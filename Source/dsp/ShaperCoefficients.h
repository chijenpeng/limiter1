// Generated offline for the noise-shaping settings - do not edit by hand.
// Error-feedback noise-shaping taps: NTF(z) = 1 - sum_k h[k] z^-k (k = 1..numTaps).
// "reduction" is the Terhardt-weighted noise reduction against a flat floor at each rate of the
// family, "boost" the maximum of |NTF|^2, "unweighted" the total noise gain 10 log10 (1 + sum h^2).
#pragma once

namespace limiter1
{
struct ShaperTaps
{
    int numTaps;
    double h[9];
};

// [family][setting]: family 0 = below 44.1 kHz, 1 = 44.1/48 kHz, 2 = 88.2/96 kHz, 3 = 176.4/192 kHz;
// setting 0 = Moderate, 1 = Normal, 2 = Ultra.
inline constexpr ShaperTaps kShaperTaps[4][3] = {
    { // below 44.1 kHz (designed at 32 kHz): gentle second-order shapers at fixed caps of 4, 6 and 8 dB
        // Moderate: reduction 2.61 dB at 32.0 kHz; boost 4.00 dB; unweighted 1.01 dB
        { 2, { 0.5060534921, -0.0788370601 } },
        // Normal: reduction 3.90 dB at 32.0 kHz; boost 6.00 dB; unweighted 2.01 dB
        { 2, { 0.7123690132, -0.2828917996 } },
        // Ultra: reduction 5.06 dB at 32.0 kHz; boost 8.00 dB; unweighted 3.49 dB
        { 2, { 0.9709504207, -0.5409340380 } },
    },
    { // 44.1 and 48 kHz
        // Moderate: reduction 6.04 dB at 44.1 kHz, 6.43 dB at 48.0 kHz; boost 3.87 dB; unweighted 1.81 dB
        { 9, { 0.6655348617, -0.0223588366, -0.2222286127, -0.0965680808, 0.0026937812, -0.0356053684, -0.0890463479, -0.0697296462, -0.0204176377 } },
        // Normal: reduction 8.50 dB at 44.1 kHz, 9.20 dB at 48.0 kHz; boost 6.56 dB; unweighted 3.80 dB
        { 9, { 1.0773685007, -0.3394838730, -0.2976781204, 0.0907033027, 0.0750738501, -0.1011935561, -0.0992717905, -0.0084985607, 0.0142694844 } },
        // Ultra: reduction 10.54 dB at 44.1 kHz, 11.39 dB at 48.0 kHz; boost 9.26 dB; unweighted 5.83 dB
        { 9, { 1.4203416625, -0.8065826905, -0.1459039187, 0.3120314909, -0.0833020631, -0.1674274218, 0.0053994231, 0.0370294546, -0.0403420710 } },
    },
    { // 88.2 and 96 kHz
        // Moderate: reduction 6.18 dB at 88.2 kHz, 6.58 dB at 96.0 kHz; boost 1.60 dB; unweighted 0.71 dB
        { 9, { 0.3212793090, 0.2133589989, 0.1276062560, 0.0379502319, -0.0300511224, -0.0529736815, -0.0490914795, -0.0498376704, -0.0349684737 } },
        // Normal: reduction 8.59 dB at 88.2 kHz, 9.22 dB at 96.0 kHz; boost 2.30 dB; unweighted 1.32 dB
        { 9, { 0.4928113129, 0.2831588459, 0.1059196417, -0.0194878310, -0.0791874251, -0.0873184050, -0.0637783640, -0.0380041165, -0.0142621777 } },
        // Ultra: reduction 10.58 dB at 88.2 kHz, 11.30 dB at 96.0 kHz; boost 3.12 dB; unweighted 1.89 dB
        { 9, { 0.6547293276, 0.2952994821, 0.0339437204, -0.0909664112, -0.1133258194, -0.0716361265, -0.0250341852, 0.0060332278, 0.0086113287 } },
    },
    { // 176.4 and 192 kHz
        // Moderate: reduction 6.29 dB at 176.4 kHz, 6.73 dB at 192.0 kHz; boost 0.86 dB; unweighted 0.40 dB
        { 9, { 0.1915224300, 0.1593398099, 0.1284758752, 0.0979389158, 0.0686551476, 0.0447916763, 0.0266899586, 0.0123456639, 0.0030677879 } },
        // Normal: reduction 8.66 dB at 176.4 kHz, 9.23 dB at 192.0 kHz; boost 1.41 dB; unweighted 0.69 dB
        { 9, { 0.2778781274, 0.2368313930, 0.1502398365, 0.1187819297, 0.0449668994, 0.0331373580, -0.0016974408, -0.0035120800, -0.0169181556 } },
        // Ultra: reduction 10.67 dB at 176.4 kHz, 11.16 dB at 192.0 kHz; boost 1.72 dB; unweighted 0.89 dB
        { 9, { 0.3561327971, 0.2452151439, 0.1605412533, 0.0944925840, 0.0438472303, 0.0010211572, -0.0284667031, -0.0422870437, -0.0312970358 } },
    },
};
} // namespace limiter1
