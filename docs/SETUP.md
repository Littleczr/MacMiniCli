# SSH setup

## 1. Prepare Windows

Install the Windows OpenSSH Client optional feature if it is missing. MacMiniCli obtains the Windows system directory with `GetSystemDirectoryW` and uses only the absolute `OpenSSH\ssh.exe` and `OpenSSH\sftp.exe` paths beneath it. It does not search your working directory or `PATH` for these executables. Missing required executables produce a JSON `setup_error` before transport starts.

Keep your SSH private keys outside this repository. The `--key` option passes only the identity path to OpenSSH; MacMiniCli does not read or print private-key contents. If you omit `--key`, OpenSSH uses its normal public-key identities or agent. Password and keyboard-interactive authentication are disabled.

## 2. Prepare the Mac

Enable Remote Login for the account you intend to use. Determine the Mac's current host/address and account username. Authorize the Windows identity's **public** key for that account using your normal SSH setup procedure. Never move the private key to the Mac or include it in a skill, chat attachment, or repository.

The chosen account determines what remote commands and files are accessible. Prepare the remote parent directory for uploads; the CLI does not create it. Use non-interactive commands. Password prompts, editors, pagers, and interactive `sudo` are unsuitable for the CLI.

## 3. Verify host identity

MacMiniCli always uses `StrictHostKeyChecking=yes`. Establish trust in Windows OpenSSH's `known_hosts` before running the CLI.

Obtain the Mac's host-key fingerprint through an independent trusted channel, such as its console or administrator. Compare it with the offered SSH host key before accepting it through your normal OpenSSH setup. A network observation alone does not establish the machine's identity.

For a new or changed host key, stop and resolve the discrepancy. Do not disable host-key checking or delete trust entries merely to suppress the error. No `known_hosts` or host-key fingerprints are bundled here.

## 4. Confirm a small operation

Run the README's `uname -s && sw_vers` example with your actual host and username. Read both the process exit code and JSON result. On a setup or authentication failure, correct the configuration rather than repeatedly invoking commands.

The CLI accepts DNS names and IPv4 addresses using letters, digits, dots, and hyphens. IPv6 address literals are not supported by its argument validator. SSH port defaults to 22. An explicitly supplied identity must be an existing regular file.

## PowerShell notes

Keep child diagnostics on stderr and JSON on stdout. In Windows PowerShell 5.1, native stderr under `$ErrorActionPreference = 'Stop'` can interrupt direct invocation before you capture `$LASTEXITCODE`. Use the README's scoped `Continue` pattern for simple commands, or use `Start-Process` with separate output files and explicit Windows argument quoting as shown in `acceptance-launcher.ps1`.

Do not merge stderr into JSON. Capture `$LASTEXITCODE` immediately after a direct invocation. Do not execute the upload and download examples consecutively and then interpret only the last exit code.

PowerShell and the remote shell are separate quoting layers. For simple literal commands, pass one PowerShell single-quoted argument and double embedded single quotes. Dynamic remote paths require correct POSIX shell quoting; do not insert untrusted text directly into shell commands. Shell commands run through the Mac account's normal SSH remote-command shell.

## Configure the skill locally

Supply these facts to your assistant through your private local settings or conversation:

| Setting | Meaning |
| --- | --- |
| Executable path | Absolute path to your built `MacMiniCli.exe` |
| Host | Your Mac's DNS name or IPv4 address |
| SSH user | The Mac account authorized for SSH |
| Port | The SSH port; 22 unless configured otherwise |
| Identity path | Optional local key path, or use normal OpenSSH identities/agent |

The public skill contains examples only. It must request missing settings instead of treating example values as your configuration.
