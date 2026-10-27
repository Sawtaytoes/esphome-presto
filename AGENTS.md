# Agent instructions

This is an ESPHome external component for the Pimoroni Presto. Keep examples
portable and free of installation credentials, real device IDs, and hostnames.

Run the host tests (`python -m unittest discover -s tests`), shared native lint,
and affected ESPHome example builds. A successful compile is not on-board
performance evidence. Report frame timings, input behavior, and scanout overruns
separately, and distinguish measurements from physical acceptance.

## Independent adversarial review

Before integrating any change, obtain review from another agent that did not
author it. Supply requirements, base/head SHAs, full diff, and verification.
Review correctness, regressions, edge cases, security, and missing coverage.
Resolve material findings and re-review substantive changes. Record reviewer,
reviewed SHAs, findings, resolutions, and verification on the PR. Passing CI and
self-review do not replace this review; unresolved material defects block merge.
Reviewers must not edit, merge, or deploy. Only an explicit owner instruction
can waive this requirement. Merge after all checks pass, then verify affected
hardware when deployment is part of the task.
