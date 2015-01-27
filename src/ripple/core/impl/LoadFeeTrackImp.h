//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_LOADFEETRACKIMP_H_INCLUDED
#define RIPPLE_LOADFEETRACKIMP_H_INCLUDED

#include <ripple/protocol/JsonFields.h>
#include <ripple/core/LoadFeeTrack.h>
#include <mutex>

namespace ripple {

class LoadFeeTrackImp : public LoadFeeTrack
{
public:
    explicit LoadFeeTrackImp (beast::Journal journal = beast::Journal())
        : m_journal (journal)
        , mLocalLoadLevel (lftReference)
        , mRemoteLoadLevel (lftReference)
        , mClusterLoadLevel (lftReference)
        , raiseCount (0)
    {
    }

    // Scale using load as well as base rate
    std::uint64_t scaleFeeLoad (std::uint64_t fee, std::uint64_t baseFee, std::uint32_t referenceFeeUnits, bool bAdmin)
    {
        static std::uint64_t midrange (0x00000000FFFFFFFF);

        bool big = (fee > midrange);

        if (big)                // big fee, divide first to avoid overflow
            fee /= referenceFeeUnits;
        else                    // normal fee, multiply first for accuracy
            fee *= baseFee;

        std::uint32_t feeFactor = std::max (mLocalLoadLevel, mRemoteLoadLevel);

        // Let admins pay the normal fee until the local load exceeds four times the remote
        std::uint32_t uRemFee = std::max(mRemoteLoadLevel, mClusterLoadLevel);
        if (bAdmin && (feeFactor > uRemFee) && (feeFactor < (4 * uRemFee)))
            feeFactor = uRemFee;

        {
            ScopedLockType sl (mLock);
            fee = mulDiv (fee, feeFactor, lftReference);
        }

        if (big)                // Fee was big to start, must now multiply
            fee *= baseFee;
        else                    // Fee was small to start, mst now divide
            fee /= referenceFeeUnits;

        return fee;
    }

    // Scale from fee units to millionths of a ripple
    std::uint64_t scaleFeeBase (std::uint64_t fee, std::uint64_t baseFee, std::uint32_t referenceFeeUnits)
    {
        return mulDiv (fee, baseFee, referenceFeeUnits);
    }

    std::uint32_t getRemoteLevel ()
    {
        ScopedLockType sl (mLock);
        return mRemoteLoadLevel;
    }

    std::uint32_t getLocalLevel ()
    {
        ScopedLockType sl (mLock);
        return mLocalLoadLevel;
    }

    std::uint32_t getLoadBase ()
    {
        return lftReference;
    }

    std::uint32_t getLoadFactor ()
    {
        ScopedLockType sl (mLock);
        return std::max(mClusterLoadLevel, std::max (mLocalLoadLevel, mRemoteLoadLevel));
    }

    void setClusterLevel (std::uint32_t level)
    {
        ScopedLockType sl (mLock);
        mClusterLoadLevel = level;
    }

    std::uint32_t getClusterLevel ()
    {
        ScopedLockType sl (mLock);
        return mClusterLoadLevel;
    }

    bool isLoadedLocal ()
    {
        // VFALCO TODO This could be replaced with a SharedData and
        //             using a read/write lock instead of a critical section.
        //
        //        NOTE This applies to all the locking in this class.
        //
        //
        ScopedLockType sl (mLock);
        return (raiseCount != 0) || (mLocalLoadLevel != lftReference);
    }

    bool isLoadedCluster ()
    {
        // VFALCO TODO This could be replaced with a SharedData and
        //             using a read/write lock instead of a critical section.
        //
        //        NOTE This applies to all the locking in this class.
        //
        //
        ScopedLockType sl (mLock);
        return (raiseCount != 0) || (mLocalLoadLevel != lftReference) || (mClusterLoadLevel != lftReference);
    }

    void setRemoteLevel (std::uint32_t f)
    {
        ScopedLockType sl (mLock);
        mRemoteLoadLevel = f;
    }

    bool raiseLocalLevel ()
    {
        ScopedLockType sl (mLock);

        if (++raiseCount < 2)
            return false;

        std::uint32_t origLevel = mLocalLoadLevel;

        if (mLocalLoadLevel < mRemoteLoadLevel)
            mLocalLoadLevel = mRemoteLoadLevel;

        mLocalLoadLevel += (mLocalLoadLevel / lftLevelIncFraction); // increment by 1/16th

        if (mLocalLoadLevel > lftLevelMax)
            mLocalLoadLevel = lftLevelMax;

        if (origLevel == mLocalLoadLevel)
            return false;

        m_journal.debug << "Local load level raised from " <<
            origLevel << " to " << mLocalLoadLevel;
        return true;
    }

    bool lowerLocalLevel ()
    {
        ScopedLockType sl (mLock);
        std::uint32_t origLevel = mLocalLoadLevel;
        raiseCount = 0;

        mLocalLoadLevel -= (mLocalLoadLevel / lftLevelDecFraction ); // reduce by 1/4

        if (mLocalLoadLevel < lftReference)
            mLocalLoadLevel = lftReference;

        if (origLevel == mLocalLoadLevel)
            return false;

        m_journal.debug << "Local load level lowered from " <<
            origLevel << " to " << mLocalLoadLevel;
        return true;
    }

    Json::Value getJson (std::uint64_t baseFee, std::uint32_t referenceFeeUnits)
    {
        Json::Value j (Json::objectValue);

        {
            ScopedLockType sl (mLock);

            // base_fee = The cost to send a "reference" transaction under no load, in millionths of a Ripple
            j[jss::base_fee] = Json::Value::UInt (baseFee);

            // load_fee = The cost to send a "reference" transaction now, in millionths of a Ripple
            j[jss::load_fee] = Json::Value::UInt (
                                mulDiv (baseFee, std::max (mLocalLoadLevel, mRemoteLoadLevel), lftReference));
        }

        return j;
    }

private:
    // VFALCO TODO Move this function to some "math utilities" file
    // compute (value)*(mul)/(div) - avoid overflow but keep precision
    std::uint64_t mulDiv (std::uint64_t value, std::uint32_t mul, std::uint64_t div)
    {
        // VFALCO TODO replace with beast::literal64bitUnsigned ()
        //
        static std::uint64_t boundary = (0x00000000FFFFFFFF);

        if (value > boundary)                           // Large value, avoid overflow
            return (value / div) * mul;
        else                                            // Normal value, preserve accuracy
            return (value * mul) / div;
    }

private:
    static const int lftReference = 256;        // 256 is the minimum/normal load factor
    static const int lftLevelIncFraction = 4;     // increase fee by 1/4
    static const int lftLevelDecFraction = 4;     // decrease fee by 1/4
    static const int lftLevelMax = lftReference * 1000000;

    beast::Journal m_journal;
    typedef std::mutex LockType;
    typedef std::lock_guard <LockType> ScopedLockType;
    LockType mLock;

    std::uint32_t mLocalLoadLevel;        // Scale factor, lftReference = normal
    std::uint32_t mRemoteLoadLevel;       // Scale factor, lftReference = normal
    std::uint32_t mClusterLoadLevel;      // Scale factor, lftReference = normal
    int raiseCount;
};

} // ripple

#endif
