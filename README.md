# MacMiniCli

Run commands and transfer files between Windows and a Mac over SSH, with structured JSON results for scripts and AI assistants.

MacMiniCli is a native Windows C++17 command-line program built on Windows OpenSSH. It supports three operations: `exec`, `upload`, and `download`. A companion [LlamaBoss skill](skills/macmini-ssh/SKILL.md) teaches an assistant how to use it through the application's PowerShell tool.

The CLI works independently of LlamaBoss and has no third-party C++ library dependency. Nothing needs to be installed on the Mac beyond a working SSH/SFTP service. This repository contains the standalone CLI and companion skill; a native LlamaBoss tool integration is outside its scope.

## Features

- One UTF-8 JSON object on stdout per normal invocation; progress and diagnostics on stderr.
- Public-key authentication and strict host-key checking.
- Explicit timeouts, bounded output capture, and local process-tree cleanup.
- Separate reporting for failure, uncertain completion, and possibly partial transfers.
- No automatic command or transfer retries.
- Built-in offline self-tests and an optional Mac round-trip acceptance launcher.

## Requirements and build

Use Windows x64 with Visual Studio 2026 or Build Tools, the MSVC **v145** C++ toolset, and a Windows SDK. Install Windows OpenSSH Client for remote operations. On the Mac, enable Remote Login for the intended account and configure trusted public-key access. See [setup instructions](docs/SETUP.md).

Open Developer PowerShell, change to the repository directory, and build:

```powershell
MSBuild.exe .\MacMiniCli.vcxproj /m /nr:false /p:Configuration=Release /p:Platform=x64
```

The executable is `build\x64\Release\MacMiniCli.exe`. This project does not include a prebuilt binary.

Run checks that do not connect to any remote host:

```powershell
& .\build\x64\Release\MacMiniCli.exe --self-test
& .\acceptance-launcher.ps1 -Mode LocalChecks
```

Capture the exit code immediately after each command; exit 0 means its checks passed. The launcher writes local evidence files into a new ignored directory.

## Quick start

Replace the example address, username, and paths with your own. `192.0.2.10` is a documentation address, not a configured or reachable Mac. Run one operation at a time.

```powershell
$mm = (Resolve-Path .\build\x64\Release\MacMiniCli.exe).Path
$common = @('--host', '192.0.2.10', '--user', 'MAC_USER', '--port', '22')
# Optional: use a specific identity instead of normal OpenSSH identities/agent.
# $common += @('--key', 'C:\Users\you\.ssh\id_ed25519')

# Separate stderr from JSON; tolerate native stderr in Windows PowerShell 5.1.
$savedPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $json = & $mm exec @common --timeout-ms 120000 -- 'uname -s && sw_vers'
    $code = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedPreference
}
$r = ($json | Out-String) | ConvertFrom-Json -ErrorAction Stop
$r
if ($code -ne 0 -or $r.success -ne $true -or $r.status -ne 'completed') {
    throw ('MacMiniCli did not report completion: ' + $r.status + ' ' + $r.error)
}
```

Upload and download use the same connection arguments and result handling:

```powershell
# Use the try/finally and JSON handling above around the chosen invocation.
$json = & $mm upload @common --timeout-ms 600000 `
    --local 'C:\path\to\input file.txt' --remote '/Users/MAC_USER/Drop/input file.txt'

# Choose a new destination; the CLI itself allows overwriting an existing file.
$json = & $mm download @common --timeout-ms 600000 `
    --remote '/Users/MAC_USER/Drop/input file.txt' --local 'C:\path\to\downloaded file.txt'
```

The local download parent directory and remote upload parent directory must already exist. Upload local paths containing `[` or `]` are rejected. Remote transfer paths containing `*`, `?`, `[` or `]` are rejected.

For `exec`, everything after `--` becomes the remote shell command. Pass it as **one argument**. In Windows PowerShell 5.1, avoid embedded double quotes in native arguments. For example:

```powershell
$json = & $mm exec @common -- 'ls -la ''/Users/MAC_USER/My Folder'''
```

For commands requiring more complex quoting, use an explicitly quoted Windows argument vector through `Start-Process`, as the acceptance launcher does. Keep stdout and stderr in separate files; do not merge them with `2>&1`.

## Results and limits

See [result semantics](docs/RESULTS.md) for JSON fields and exit codes. The defaults are 120 seconds for commands and 600 seconds for transfers; `--timeout-ms` accepts 1 through 3,600,000 milliseconds. Each captured child output stream retains at most 1 MiB while excess output continues to be drained.

An operation is successful only when the CLI process exits 0 and JSON reports `success:true`, `status:"completed"`, and `exit_code:0`. A disconnected or timed-out command may already have executed. A failed transfer may leave partial data. Stopping local SSH does not prove the remote process stopped.

**The CLI is a transport tool, not an authorization boundary.** It can execute arbitrary commands as the configured Mac user and may overwrite files during transfers. Assistant approval, credential-file exclusion, permitted folders, and choosing new destinations must be enforced by the caller. The skill provides instructions for those decisions; it does not add enforcement inside the executable.

## LlamaBoss skill

Import the `skills/macmini-ssh` folder using your LlamaBoss skill workflow, or copy it to a `macmini-ssh` directory in your configured Skills folder. The skill has no bundled executable and no personal connection settings. Provide your executable path, Mac host, SSH username, port, and optional identity path locally before using it.

Example requests after configuration:

- “Check the Mac's macOS version and uptime.”
- “Upload this project file to the Mac's Drop folder.”
- “Download the generated report to a new local file.”

The skill requires a Windows execution tool capable of running PowerShell. Import alone does not establish SSH connectivity or grant permission to a remote machine.

## Optional live acceptance

After configuring SSH trust and authentication, intentionally run:

```powershell
& .\acceptance-launcher.ps1 -Mode Acceptance `
    -HostAddress '192.0.2.10' -SshUser 'MAC_USER' -Port 22
# Add -KeyPath 'C:\Users\you\.ssh\id_ed25519' if needed.
```

This checks `Darwin` and the expected username, uploads a unique 33-byte test file under `/tmp`, downloads it to a unique local path, and compares bytes and SHA-256 hashes. It stops on the first failed step. It deliberately leaves the test files and local evidence for inspection. Evidence contains connection details and captured output; keep it private.

## Publishing and validation

See [publishing notes](docs/PUBLISHING.md) for the package's validation status and the remaining Windows checks before a release. Historical integration records and machine-specific evidence are not included.

## License

MIT. See [LICENSE](LICENSE). The license covers the CLI, documentation, launcher, and bundled skill. Windows OpenSSH is an external prerequisite with its own licensing.
