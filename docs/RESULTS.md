# Results and process behavior

## JSON contract

Normal operations, invalid invocations, and `--help` write one UTF-8 JSON object and a newline to stdout. Progress and CLI diagnostics use stderr. Child stdout and stderr are captured into the JSON fields. `--self-test` also emits one JSON object, but uses a check-list schema rather than the operation result schema. Internal `--test-child-*` modes are self-test fixtures, not public operation interfaces.

Operation schema version 1:

| Field | Meaning |
| --- | --- |
| `schema_version` | Currently `1` |
| `operation` | Requested operation, or diagnostic operation such as `help` |
| `success`, `status` | Completion classification |
| `exit_code` | OpenSSH child status when available; otherwise JSON `null` |
| `timed_out` | The operation exceeded its local deadline |
| `outcome_uncertain` | Remote execution or transfer completion is unknown |
| `partial_possible` | A failed or interrupted transfer may have left partial data |
| `local_termination_confirmed` | Local process termination was confirmed after timeout cleanup; does not establish remote termination |
| `stdout`, `stderr` | Captured child output, each limited to 1 MiB retained data |
| `stdout_truncated`, `stderr_truncated` | Whether captured output exceeded its retention cap |
| `stdin_error`, `capture_error`, `cleanup_error` | Local runner diagnostic strings |
| `duration_ms` | Measured operation duration |
| `error` | Human-readable failure details, possibly with a cautious SSH setup hint |

Excess child output is still drained after the cap to avoid deadlock. Malformed child-output UTF-8 is replaced with U+FFFD so the emitted JSON remains valid.

## Exit codes

The CLI process status and JSON `exit_code` describe different processes:

| CLI process exit | Meaning |
| --- | --- |
| `0` | Successful operation, successful help, or passing self-tests |
| `1` | Non-timeout operation failure or failing self-tests |
| `2` | Invalid command-line arguments |
| `124` | Operation timeout |

For `exec`, a received child exit 0 indicates success; exits 1–254 are `remote_command_failed` and retain that status in JSON. Exit 255, or an unavailable child exit status after dispatch, is uncertain: the command may have run or may still be running. When MacMiniCli has terminated OpenSSH itself, JSON does not invent a remote exit code.

For upload and download, child exit 0 indicates completion. Nonzero SFTP exits are `transfer_failed` with `outcome_uncertain:true` and `partial_possible:true`. These flags do not prove whether the destination is complete, partial, or absent. Pre-launch setup failures are reported separately. Capture, stdin, or cleanup failures are not treated as successful operations.

Check process exit 0, `success:true`, `status:"completed"`, and `exit_code:0` together before reporting an operation as completed. Inspect truncation and diagnostic fields before claiming that captured output is complete.

## Timeouts and cleanup

Default deadlines are 120,000 ms for `exec` and 600,000 ms for transfers. `--timeout-ms` accepts 1 through 3,600,000. SSH also uses a 15-second connection timeout, a 15-second keepalive interval, and a keepalive count of three.

The runner launches the system OpenSSH executables directly with `CreateProcessW`, quoted Windows arguments, explicitly inherited handles, overlapped pipes, and concurrent stdout/stderr draining. SFTP batch input is delivered under the same operation deadline. Timeout cleanup uses a Windows Job Object to terminate the contained local process tree, cancels pending I/O, and uses bounded cleanup waits.

Killing local SSH does not establish whether the remote command stopped. There are no automatic retries. After an uncertain result, verify remote state through an authorized read-only operation before considering another mutation.

## Transfer paths and caller responsibilities

SFTP paths are escaped and quoted in the batch language. Newlines and control characters are rejected. Remote glob characters `* ? [ ]` are rejected. Upload local paths are made absolute, backslashes are converted to forward slashes, and paths containing `[` or `]` are rejected to avoid local glob expansion. Download local paths are quoted as supplied rather than passed through upload normalization.

Downloads require an existing local parent directory and a file destination, and **may replace existing files**. Uploads may also replace remote files. A caller's preflight existence check does not provide atomic no-overwrite protection against concurrent changes. Use destinations that cannot collide, avoid concurrent writers, and implement stronger staging or no-overwrite policy in the caller if required.

The executable does not enforce assistant approvals, folder grants, credential-file exclusion, or a remote command allowlist. Those are caller responsibilities. The companion skill tells the assistant to respect authorization, avoid credential stores, and choose fresh destinations, but instructions are not executable security enforcement.
