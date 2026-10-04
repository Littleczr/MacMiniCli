[CmdletBinding()]
param(
    [ValidateSet('LocalChecks', 'Acceptance')]
    [string]$Mode = 'LocalChecks',
    [string]$RepoRoot = $PSScriptRoot,
    [string]$EvidenceDirectory,
    [string]$HostAddress,
    [string]$SshUser,
    [ValidateRange(1, 65535)]
    [int]$Port = 22,
    [string]$KeyPath
)

$ErrorActionPreference = 'Stop'
if ($Mode -eq 'Acceptance') {
    if ([string]::IsNullOrWhiteSpace($HostAddress) -or [string]::IsNullOrWhiteSpace($SshUser)) {
        throw 'Acceptance requires explicit -HostAddress and -SshUser values.'
    }
    if ($KeyPath -and -not (Test-Path -LiteralPath $KeyPath -PathType Leaf)) {
        throw 'The configured -KeyPath must name an existing identity file. Omit it to use normal OpenSSH identities or the agent.'
    }
}
$CliPath = Join-Path $RepoRoot 'build\x64\Release\MacMiniCli.exe'
if (-not (Test-Path -LiteralPath $CliPath -PathType Leaf)) { throw "CLI executable not found: $CliPath" }
if (-not $EvidenceDirectory) {
    $prefix = if ($Mode -eq 'LocalChecks') { 'local-checks-' } else { 'acceptance-evidence-' }
    $EvidenceDirectory = Join-Path $RepoRoot ($prefix + [guid]::NewGuid().ToString('N'))
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw "Refusing to reuse existing evidence directory: $EvidenceDirectory" }
New-Item -ItemType Directory -Path $EvidenceDirectory -ErrorAction Stop | Out-Null
$script:CliPath = $CliPath
$script:EvidenceDirectory = $EvidenceDirectory
$script:ExitCodePath = Join-Path $EvidenceDirectory 'process-exit-codes.txt'
[System.IO.File]::WriteAllText($script:ExitCodePath, '', [System.Text.UTF8Encoding]::new($false))

function ConvertTo-WindowsArgument {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append([char]34)
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) { $slashes++; continue }
        if ($character -eq [char]34) {
            for ($i = 0; $i -lt (2 * $slashes + 1); $i++) { [void]$builder.Append([char]92) }
            [void]$builder.Append([char]34)
            $slashes = 0
            continue
        }
        for ($i = 0; $i -lt $slashes; $i++) { [void]$builder.Append([char]92) }
        [void]$builder.Append($character)
        $slashes = 0
    }
    for ($i = 0; $i -lt (2 * $slashes); $i++) { [void]$builder.Append([char]92) }
    [void]$builder.Append([char]34)
    return $builder.ToString()
}

function Invoke-CliCaptured {
    param(
        [Parameter(Mandatory)][string]$OperationName,
        [Parameter(Mandatory)][string[]]$Arguments
    )
    $stdoutPath = Join-Path $script:EvidenceDirectory ($OperationName + '.stdout.json')
    $stderrPath = Join-Path $script:EvidenceDirectory ($OperationName + '.stderr.txt')
    if ((Test-Path -LiteralPath $stdoutPath) -or (Test-Path -LiteralPath $stderrPath)) {
        throw "Refusing to overwrite captured output for $OperationName."
    }
    $quotedArguments = @($Arguments | ForEach-Object { ConvertTo-WindowsArgument -Value ([string]$_) })
    $argumentLine = $quotedArguments -join ' '
    $process = Start-Process -FilePath $script:CliPath -ArgumentList $argumentLine -Wait -PassThru -NoNewWindow -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    # Capture the actual child exit code before reading or parsing stdout.
    $exitCode = $process.ExitCode
    [System.IO.File]::AppendAllText($script:ExitCodePath, ($OperationName + '=' + $exitCode + "`n"), [System.Text.UTF8Encoding]::new($false))
    $stdoutText = [System.IO.File]::ReadAllText($stdoutPath, [System.Text.Encoding]::UTF8)
    $json = $null
    $parseError = $null
    try { $json = $stdoutText | ConvertFrom-Json -ErrorAction Stop } catch { $parseError = $_.Exception.Message }
    [pscustomobject]@{
        Operation = $OperationName
        Arguments = $Arguments
        ProcessExitCode = $exitCode
        StdoutPath = $stdoutPath
        StderrPath = $stderrPath
        StderrBytes = (Get-Item -LiteralPath $stderrPath).Length
        StdoutText = $stdoutText
        Json = $json
        JsonParseError = $parseError
    }
}

if ($Mode -eq 'LocalChecks') {
    $help = Invoke-CliCaptured -OperationName 'help-corrected' -Arguments @('--help')
    $invalid = Invoke-CliCaptured -OperationName 'invalid-corrected' -Arguments @('not-a-real-operation')
    $helpValid = ($help.ProcessExitCode -eq 0 -and $null -ne $help.Json -and -not $help.JsonParseError)
    $invalidValid = ($invalid.ProcessExitCode -ne 0 -and $null -ne $invalid.Json -and -not $invalid.JsonParseError -and $invalid.Json.success -eq $false)
    $results = [ordered]@{
        powershell_version = $PSVersionTable.PSVersion.ToString()
        powershell_edition = $PSVersionTable.PSEdition
        error_action_preference = [string]$ErrorActionPreference
        native_command_use_error_action_preference = if (Test-Path Variable:PSNativeCommandUseErrorActionPreference) { [string]$PSNativeCommandUseErrorActionPreference } else { 'not-defined-in-this-PowerShell' }
        launcher = 'Start-Process -Wait -PassThru with separate stdout/stderr redirection; quoted Windows command line'
        help = [ordered]@{ process_exit_code=$help.ProcessExitCode; valid_json=($null -ne $help.Json -and -not $help.JsonParseError); stderr_bytes=$help.StderrBytes; stdout_file=$help.StdoutPath; stderr_file=$help.StderrPath; parse_error=$help.JsonParseError }
        invalid_invocation = [ordered]@{ process_exit_code=$invalid.ProcessExitCode; valid_json=($null -ne $invalid.Json -and -not $invalid.JsonParseError); json_success=if ($invalid.Json) { $invalid.Json.success } else { $null }; stderr_bytes=$invalid.StderrBytes; stdout_file=$invalid.StdoutPath; stderr_file=$invalid.StderrPath; parse_error=$invalid.JsonParseError }
        help_check_passed = $helpValid
        invalid_check_passed = $invalidValid
    }
    $resultsPath = Join-Path $EvidenceDirectory 'local-verification.json'
    [System.IO.File]::WriteAllText($resultsPath, (($results | ConvertTo-Json -Depth 8) + "`n"), [System.Text.UTF8Encoding]::new($false))
    Write-Output ('Local launcher evidence: ' + $EvidenceDirectory)
    Write-Output ('--help: exit=' + $help.ProcessExitCode + '; valid JSON=' + ($null -ne $help.Json -and -not $help.JsonParseError) + '; stderr bytes=' + $help.StderrBytes)
    Write-Output ('invalid invocation: exit=' + $invalid.ProcessExitCode + '; valid failure JSON=' + $invalidValid + '; stderr bytes=' + $invalid.StderrBytes)
    if (-not $helpValid -or -not $invalidValid) { throw "Corrected launcher local checks failed; inspect $resultsPath" }
    exit 0
}

# Acceptance mode is deliberately sequential: any failed check throws before the next operation.
$trialId = [guid]::NewGuid().ToString('N')
$source = Join-Path $env:TEMP "MacMiniCli acceptance & user's payload source-$trialId.txt"
$downloaded = Join-Path $env:TEMP "MacMiniCli acceptance & user's payload downloaded-$trialId.txt"
$remote = "/tmp/MacMiniCli acceptance & user's payload-$trialId.txt"
if ((Test-Path -LiteralPath $source) -or (Test-Path -LiteralPath $downloaded) -or $source -eq $downloaded) { throw 'Unique source/download path precondition failed.' }
[System.IO.File]::WriteAllBytes($source, [System.Text.Encoding]::UTF8.GetBytes("MacMiniCli acceptance payload v1`n"))
$summaryPath = Join-Path $EvidenceDirectory 'acceptance-summary.json'
$summary = [ordered]@{
    status = 'RUNNING'
    host = $HostAddress
    port = $Port
    user = $SshUser
    identity_path = $KeyPath
    remote_path = $remote
    local_source = $source
    local_download = $downloaded
    cli_deadlines_ms = [ordered]@{ exec=120000; upload=600000; download=600000 }
    operations = [ordered]@{ exec='PENDING'; upload='PENDING'; download='PENDING' }
    process_exit_codes = [ordered]@{ exec=$null; upload=$null; download=$null }
    source_byte_count = $null
    downloaded_byte_count = $null
    bytes_identical = $null
    source_sha256 = $null
    downloaded_sha256 = $null
}
function Save-AcceptanceSummary {
    [System.IO.File]::WriteAllText($summaryPath, (($summary | ConvertTo-Json -Depth 8) + "`n"), [System.Text.UTF8Encoding]::new($false))
}
function Assert-CliSuccess {
    param($Result, [string]$Name)
    if ($Result.ProcessExitCode -ne 0 -or $null -eq $Result.Json -or $Result.Json.success -ne $true -or $Result.Json.status -ne 'completed' -or $Result.Json.exit_code -ne 0) {
        $summary.operations[$Name] = 'FAILED_RESULT_CHECK'
        $summary.status = 'STOPPED_AT_' + $Name.ToUpperInvariant()
        Save-AcceptanceSummary
        throw "$Name failed a required process/JSON check; see the per-operation stdout and stderr files."
    }
}
$common = @('--host',$HostAddress,'--user',$SshUser,'--port',[string]$Port)
if ($KeyPath) { $common += @('--key',$KeyPath) }
$summary.operations.exec = 'RUNNING'; Save-AcceptanceSummary
$exec = Invoke-CliCaptured -OperationName 'exec' -Arguments (@('exec') + $common + @('--timeout-ms','120000','--','uname -s && id -un'))
$summary.process_exit_codes.exec = $exec.ProcessExitCode
$summary.operations.exec = 'RESULT_CAPTURED'; Save-AcceptanceSummary
Assert-CliSuccess -Result $exec -Name 'exec'
$identity = @($exec.Json.stdout -split '\r?\n' | Where-Object { $_ -ne '' })
if ($identity.Count -ne 2 -or $identity[0] -ne 'Darwin' -or $identity[1] -ne $SshUser) { $summary.operations.exec='FAILED_REMOTE_IDENTITY'; $summary.status='STOPPED_AT_EXEC'; Save-AcceptanceSummary; throw 'Exec did not confirm Darwin and the expected SSH username.' }
$summary.operations.exec='PASSED'; Save-AcceptanceSummary
$summary.operations.upload='RUNNING'; Save-AcceptanceSummary
$upload = Invoke-CliCaptured -OperationName 'upload' -Arguments (@('upload') + $common + @('--timeout-ms','600000','--local',$source,'--remote',$remote))
$summary.process_exit_codes.upload = $upload.ProcessExitCode
$summary.operations.upload='RESULT_CAPTURED'; Save-AcceptanceSummary
Assert-CliSuccess -Result $upload -Name 'upload'
$summary.operations.upload='PASSED'; Save-AcceptanceSummary
$summary.operations.download='RUNNING'; Save-AcceptanceSummary
$download = Invoke-CliCaptured -OperationName 'download' -Arguments (@('download') + $common + @('--timeout-ms','600000','--remote',$remote,'--local',$downloaded))
$summary.process_exit_codes.download = $download.ProcessExitCode
$summary.operations.download='RESULT_CAPTURED'; Save-AcceptanceSummary
Assert-CliSuccess -Result $download -Name 'download'
$summary.operations.download='PASSED'; Save-AcceptanceSummary
$sourceBytes = [System.IO.File]::ReadAllBytes($source)
$downloadBytes = [System.IO.File]::ReadAllBytes($downloaded)
$summary.source_byte_count = $sourceBytes.Length
$summary.downloaded_byte_count = $downloadBytes.Length
$summary.bytes_identical = [Convert]::ToBase64String($sourceBytes) -ceq [Convert]::ToBase64String($downloadBytes)
$summary.source_sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
$summary.downloaded_sha256 = (Get-FileHash -LiteralPath $downloaded -Algorithm SHA256).Hash
$summary.status = 'COMPARISON_RECORDED_BEFORE_ASSERTION'
Save-AcceptanceSummary
if (-not $summary.bytes_identical -or $summary.source_sha256 -ne $summary.downloaded_sha256) { $summary.status='FAILED_BYTE_OR_HASH_COMPARISON'; Save-AcceptanceSummary; throw 'Byte identity or SHA-256 comparison failed.' }
$summary.status='PASSED'; Save-AcceptanceSummary
Write-Output ('Acceptance passed; evidence: ' + $EvidenceDirectory)
Write-Output ('Remote identity: ' + ($identity -join '/'))
Write-Output ('Process exit codes: exec=' + $exec.ProcessExitCode + ', upload=' + $upload.ProcessExitCode + ', download=' + $download.ProcessExitCode)
Write-Output ('Byte counts: source=' + $summary.source_byte_count + ', download=' + $summary.downloaded_byte_count + '; identical=' + $summary.bytes_identical)
Write-Output ('SHA-256: source=' + $summary.source_sha256 + '; download=' + $summary.downloaded_sha256)
