# Public package and release checks

## Included scope

This package contains the standalone CLI source/project, acceptance launcher, public setup/result documentation, MIT license, and portable `macmini-ssh` skill. It contains no compiled binary, private connection configuration, native LlamaBoss integration sources, or private test evidence.

The CLI source `main.cpp` is byte-for-byte unchanged from the imported public candidate. Public preparation changed only these publishing notes after validation. The acceptance launcher requires explicit host/user settings for live mode, supports a selected port, and allows normal OpenSSH identities/agent when no key path is supplied. Its process capture, argument quoting, and sequential round-trip checks are retained.

The skill has no personal host, username, key location, executable location, or machine facts. It describes assistant-side authorization and fresh-destination precautions without attributing them to CLI enforcement. Historical development records and host-key fingerprints are not included.

## Windows validation — 10/4/2026

Validation used a fresh isolated extraction of the imported public ZIP on Windows. Real connection settings, identity paths, command output, logs, and acceptance evidence were kept outside the public package. Private-key contents were never read or copied.

Results:

- **Release x64 build: PASS.** The installed Visual Studio 2026 MSBuild and v145 toolset built `MacMiniCli.vcxproj` with process exit 0, 0 warnings, and 0 errors. Portable command: `MSBuild.exe .\MacMiniCli.vcxproj /m /nr:false /p:Configuration=Release /p:Platform=x64`.
- **Built-in self-test: PASS.** `& .\build\x64\Release\MacMiniCli.exe --self-test` exited 0; its JSON parsed successfully and reported 33 checks, 0 failures.
- **Launcher local checks: PASS.** `& .\acceptance-launcher.ps1 -Mode LocalChecks -EvidenceDirectory <private-evidence-dir>` exited 0. Help returned valid JSON with exit 0; the deliberate invalid invocation returned valid failure JSON with exit 2.
- **Configuration handling: PASS.** Acceptance mode without host/user failed before creating evidence or dispatching a remote operation. Port validation accepts 1–65535 and defaults to 22. A nonexistent key path failed before dispatch. Omitting `-KeyPath` leaves `--key` out so normal OpenSSH identities/agent can be used, matching the README.
- **Live acceptance: PASS.** One intentional run used private local settings, strict host-key checking, a unique remote `/tmp` name, and fresh local paths. Identity verification, upload, download, and all three CLI process exits passed. The 33-byte payload matched byte-for-byte and the source/download SHA-256 values matched. No mutation was automatically retried.
- **Bundled skill read-only exercise: PASS.** A small non-interactive read-only Mac command completed with CLI process/remote exit 0, parsed JSON, `success:true`, `status:"completed"`, and no uncertainty, partial-transfer flag, or timeout.
- **Skill transfer review: PASS.** The instructions match observed transfer behavior and correctly separate executable enforcement from caller precautions. The executable handles strict host-key/public-key transport, argument/path validation, result semantics, and uncertainty reporting. The caller remains responsible for authorization, credential exclusion, fresh destinations, overwrite avoidance, and the no-automatic-retry policy.
- **Public-content audit: PASS.** All 7 project file references and all 5 relative Markdown links resolve. The MIT license text is present. The 11 public files contain no detected personal connection settings, private LAN address, personal username/path, private-key block, host public-key material, or host fingerprint. Build output and private validation evidence are excluded from the release ZIP.
- **Source preservation: PASS.** Imported and final `main.cpp` SHA-256: `62D716B55FB3F8DB3A9114EA7A2582A15B11A00D702610A18852C055E6D3CBFC`; the file was not changed.

## Remaining publication limitations

- No GitHub repository, commit, tag, release, or release binary has been created by this validation.
- The live acceptance launcher deliberately leaves its uniquely named local and remote trial files for inspection; they are not part of this public package. Clean them up separately only when intentionally authorized.
- A future release binary should be built from the tagged source, rechecked, and published with its own SHA-256.
- Inspect the Git staging list before publication. Keep private settings, SSH material, acceptance evidence, generated logs, and build output out. Custom evidence directory names are not automatically covered by every ignore rule.

Suggested repository description:

> Native Windows CLI for SSH commands and SFTP transfers to a Mac, with JSON results and a companion LlamaBoss skill.

Suggested topics: `ssh`, `sftp`, `windows`, `macos`, `cpp`, `cli`, `llamaboss`.
