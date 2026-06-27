## qB lt-retention narrow overlay

This directory keeps the retained `torrentstatusquery.h` override used by the narrow steady-state refresh policy.

The rest of the narrow-payload behavior lives in `0005-feat-narrow-steady-state-payload-policy.patch`, which:

- includes `torrentstatusquery.h` in `sessionimpl.cpp` and `torrentimpl.cpp`
- switches refresh and status queries to the narrow steady-state path
- wires `torrentstatusquery.h` into `src/base/CMakeLists.txt`

Keeping the small source override here and the broader behavior in the patch series keeps the narrow-payload input easy to audit.
