// Threshold and Ceiling with the Link coupling: while linked, moving either control moves the
// other by the same number of dB. The host's parameters are the requested values; what the
// limiter gets are the effective ones, and when the coupling moves a control the caller is
// asked to write the effective value back to its parameter so that the host, the UI and the
// saved state all agree. Turning Link on or off takes the current pair as the baseline, so it
// never jumps the gain. No JUCE dependency.
#pragma once

#include <algorithm>
#include <cmath>

namespace limiter1
{
class LinkedControls
{
public:
    struct Result
    {
        float thresholdDb, ceilingDb;      // effective values for the limiter
        bool writeThreshold, writeCeiling; // true: push the effective value back to the parameter
    };

    void setRange (float low, float high) noexcept { lo = low; hi = high; }

    void reset (float thresholdDb, float ceilingDb, bool linked) noexcept
    {
        threshold = Control { thresholdDb, thresholdDb };
        ceiling = Control { ceilingDb, ceilingDb };
        wasLinked = linked;
    }

    /** Once per block with the parameters' current values. */
    Result update (float requestedThreshold, float requestedCeiling, bool linked) noexcept
    {
        const bool thresholdMoved = threshold.observe (requestedThreshold);
        const bool ceilingMoved = ceiling.observe (requestedCeiling);
        const bool relinked = linked != wasLinked;
        wasLinked = linked;
        bool writeThreshold = false, writeCeiling = false;

        if (relinked || ! linked || (thresholdMoved && ceilingMoved))
        {
            // Not coupled: the pair as it stands becomes the baseline, so a Link change never
            // jumps, and a pair changed at once (a preset, two automation lanes) is taken as is.
            threshold.accept();
            ceiling.accept();
        }
        else if (thresholdMoved)
        {
            const float move = threshold.requested - threshold.previous;
            threshold.accept();
            writeCeiling = ceiling.couple (move, lo, hi);
        }
        else if (ceilingMoved)
        {
            const float move = ceiling.requested - ceiling.previous;
            ceiling.accept();
            writeThreshold = threshold.couple (move, lo, hi);
        }

        return { threshold.effective, ceiling.effective, writeThreshold, writeCeiling };
    }

    float effectiveThreshold() const noexcept { return threshold.effective; }
    float effectiveCeiling() const noexcept { return ceiling.effective; }

private:
    static constexpr float kEpsilon = 1.0e-4f;
    static constexpr int kPendingTimeout = 64;   // blocks to wait for a written value to come back

    struct Control
    {
        float requested = 0.0f, effective = 0.0f;
        float previous = 0.0f;         // the requested value the last change was measured against
        float pending = 0.0f;          // a value written back to the parameter, not yet seen
        bool hasPending = false;
        int pendingAge = 0;

        Control() = default;
        Control (float req, float eff) : requested (req), effective (eff), previous (req) {}

        /** Records the parameter's value; true when the user (or the host) moved it. */
        bool observe (float value) noexcept
        {
            requested = value;
            if (hasPending)
            {
                if (std::abs (value - pending) <= kEpsilon)
                {
                    hasPending = false;      // our own write came back: not a move
                    previous = value;
                }
                else if (++pendingAge > kPendingTimeout)
                {
                    hasPending = false;      // it never arrived: follow the host from here
                    previous = value;
                    effective = value;
                }
                return false;                // until then the parameter still shows the old value
            }
            return std::abs (value - previous) > kEpsilon;
        }

        void accept() noexcept
        {
            if (! hasPending)
            {
                effective = requested;
                previous = requested;
            }
        }

        /** Moves the effective value by `move` dB; true when the parameter must be updated. */
        bool couple (float move, float low, float high) noexcept
        {
            const float next = std::clamp (effective + move, low, high);
            if (std::abs (next - effective) <= kEpsilon)
                return false;
            effective = next;
            pending = next;
            hasPending = true;
            pendingAge = 0;
            return true;
        }
    };

    Control threshold, ceiling;
    bool wasLinked = false;
    float lo = -30.0f, hi = 0.0f;
};
} // namespace limiter1
