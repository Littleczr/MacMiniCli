---
name: macmini-ssh
description: Run non-interactive commands on a user-configured Mac and upload or download files over SSH/SFTP using MacMiniCli.exe from Windows PowerShell. Use when the user asks to check or work on that Mac, send a file to it, or retrieve a file from it through LlamaBoss or another assistant with a Windows PowerShell execution tool.
---

# Mac over SSH with MacMiniCli

Use the Windows PowerShell execution tool to invoke the standalone `MacMiniCli.exe`. Respect the application's approval workflow and the user's authorization for the requested task. Do not assume that this skill or the executable grants access to a machine or guarantees an approval card.

## Resolve configuration

Obtain the absolute executable path, Mac host/DNS name or IPv4 address, SSH username, port, and authentication choice from the user's private settings or conversation. Use port 22 unless configured otherwise. Obtain an optional local identity path, or use normal OpenSSH public-key identities/agent by omitting `--key`.

Ask for missing connection settings. Treat every address, username, and path below as an example, never as a saved connection. Do not guess the Mac's hardware, OS version, shell, or installed applications; query relevant facts when needed. Keep real connection details out of public skill copies and repositories.

Use public-key authentication only. Pass an identity path without reading, printing, copying, attaching, or uploading its contents. Do not use private keys or credential-store files as transfer payloads.

## Construct one invocation

Replace these illustrative values with the resolved private settings before executing. Choose exactly one operation per call. Capture its process exit code immediately and parse its stdout as JSON.

```powershell
$mm = 'C:\path\to\MacMiniCli\build\x64\Release\MacMiniCli.exe'
$common = @('--host', '192.0.2.10', '--user', 'MAC_USER', '--port', '22')
# Add only if the user configured a specific identity:
# $common += @('--key', 'C:\Users\you\.ssh\id_ed25519')

$savedPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $json = & $mm exec @common --timeout-ms 120000 -- 'uptime && sw_vers'
    $code = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedPreference
}
$r = ($json | Out-String) | ConvertFrom-Json -ErrorAction Stop
"exit=$code success=$($r.success) status=$($r.status) child_exit=$($r.exit_code) uncertain=$($r.outcome_uncertain) partial=$($r.partial_possible) timed_out=$($r.timed_out)"
if ($r.stdout) { $r.stdout }
if ($r.stderr) { 'stderr: ' + $r.stderr }
if ($r.error) { 'error: ' + $r.error }
```

For upload or download, substitute one of these invocations inside the same `try` block and capture `$LASTEXITCODE` immediately afterward:

```powershell
$json = & $mm upload @common --timeout-ms 600000 `
    --local 'C:\path\to\input file.txt' --remote '/Users/MAC_USER/Drop/input file.txt'

$json = & $mm download @common --timeout-ms 600000 `
    --remote '/Users/MAC_USER/Drop/input file.txt' --local 'C:\path\to\new file.txt'
```

- Tolerate native stderr with the scoped `Continue` pattern in Windows PowerShell 5.1; restore the prior preference afterward. Do not merge stderr with stdout using `2>&1`. CLI progress belongs on stderr; stdout contains one JSON result.
- Pass the operation (`exec`, `upload`, or `download`) before the common options.
- Pass the entire remote command after `--` as one argument. For simple commands in Windows PowerShell 5.1, avoid embedded double quotes in native arguments. Use doubled single quotes inside a PowerShell literal: `'ls -la ''/Users/MAC_USER/My Folder'''` sends `ls -la '/Users/MAC_USER/My Folder'` to the Mac.
- Escape dynamically supplied remote shell values using POSIX single-quote rules; do not concatenate untrusted text into a command. For commands requiring embedded double quotes or complex Windows argument handling, use an explicitly quoted argument vector with `Start-Process` and separate stdout/stderr files, following the repository's acceptance launcher. Do not silently change command meaning to work around quoting.
- Keep commands non-interactive: no editors, password prompts, pagers, or interactive `sudo`. Set an appropriate timeout for known long operations. Allow the outer PowerShell tool enough time for the operation deadline and cleanup.
- Reject remote transfer paths containing wildcard characters `* ? [ ]` or control characters. Reject upload local paths containing `[` or `]`; if needed, create an authorized temporary copy with a fresh safe name, transfer it, and delete only that owned copy afterward. Do not rename or delete the original.
- Use existing parent directories. Prefer absolute local download paths because the CLI does not normalize downloads the same way as uploads.
- If syntax appears incompatible, inspect `--help` once before changing the call. Do not rerun a failed remote mutation automatically.

## Apply caller-side precautions

The CLI executes arbitrary commands as the Mac account and allows transfer overwrites. It does not enforce folder grants, credential exclusion, per-operation approvals, or atomic no-overwrite behavior. Apply these precautions in the assistant's workflow:

- Confirm the operation and target are within the user's request and the execution tool's permitted folders. Ask only for missing authorization needed for the concrete action; reuse authorization already provided for that action.
- Never upload a private key, a file from an SSH credential directory, or another credential store. Check the selected payload path and exclude the configured identity. Do not claim that a path check protects against every alias or concurrent replacement.
- Choose a fresh local download destination. Check `Test-Path -LiteralPath` before dispatch; if it exists, choose a new name. Use a unique name and avoid concurrent writers. Treat this as a precaution, not an atomic no-overwrite guarantee. If stronger protection is required, stop and arrange caller-enforced staging or another suitable transfer method.
- Choose a new remote upload destination unless the user authorized replacing that specific file. Check existence with an authorized read-only remote command when needed. Recognize that a check can race with a later upload.
- Do not delete, overwrite, or otherwise mutate remote files beyond the user's authorized task.
- Leave strict host-key checking enabled. On a new/changed key error, stop and request that the user verify and configure trust independently. Do not edit `known_hosts` or disable verification to bypass the error.
- Do not automatically retry commands or transfers. On an uncertain result, describe it and verify remote state through an authorized read-only check before proposing another mutation.

## Interpret and report results

Report completion only when the process exit is 0 and JSON has `success:true`, `status:"completed"`, and `exit_code:0`. Treat malformed or missing JSON, capture/cleanup errors, or contradictory fields as failures requiring inspection.

For `exec`, a received child exit 1–254 is a remote command failure. Exit 255 or an unavailable status after dispatch means completion is uncertain. A timeout can terminate local SSH without proving that the Mac's command stopped. For SFTP, a failed transfer may leave complete, partial, or absent data. State `outcome_uncertain` and `partial_possible` explicitly when true; do not claim success or assume a repeat is safe.

Summarize the requested operation, process/child status, relevant trimmed output, and transfer paths. Inspect `stdout_truncated`, `stderr_truncated`, `stdin_error`, `capture_error`, and `cleanup_error`; do not claim output is complete when truncated. Avoid reproducing credentials accidentally present in command output or diagnostics.

For important transfers, verify SHA-256 when authorized: use `Get-FileHash -LiteralPath` locally and an `exec` operation running `shasum -a 256` with a correctly quoted remote path. Report whether integrity was actually verified.
