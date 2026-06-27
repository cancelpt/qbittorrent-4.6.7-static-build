This directory is for optional local or release-candidate binaries.

Repository policy:

- The root `.gitignore` keeps everything here ignored except this README.
- Do not commit binaries to the main source branch.
- Publish distribution bundles through GitHub Release assets instead.
- Expected release bundle: the staged binary, `SHA256SUMS`, and optional `SHA256SUMS.asc`.
