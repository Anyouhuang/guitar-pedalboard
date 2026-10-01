# Publishes the web version to GitHub Pages:  https://<your-account>.github.io/<repo>/
# A page served from its own HTTPS address may use the microphone / audio interface (claude.ai pages may not).
#
# First time only, log in:   tools\gh\bin\gh.exe auth login --web
# Then:                      powershell -ExecutionPolicy Bypass -File web\deploy-github.ps1
#
# Only the built page (index.html, worklet.js) and the repository's README (web\github\README.md with its
# images) are uploaded, through the GitHub API; your git settings aren't touched.
param([string]$Repo = "guitar-pedalboard")
$ErrorActionPreference = "Continue" # native tools report through exit codes, checked below

$root = Split-Path $PSScriptRoot
$gh = Join-Path $root "tools\gh\bin\gh.exe"

& $gh auth status *> $null
if ($LASTEXITCODE -ne 0) { Write-Host "Not logged in to GitHub. Run:  tools\gh\bin\gh.exe auth login --web" -ForegroundColor Red; exit 1 }

Write-Host "== Building the web version" -ForegroundColor Cyan
node (Join-Path $PSScriptRoot "build.mjs")
if ($LASTEXITCODE -ne 0) { Write-Host "build failed" -ForegroundColor Red; exit 1 }

$owner = (& $gh api user --jq .login).Trim()
$full = "$owner/$Repo"

& $gh repo view $full *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "== Creating public repository $full" -ForegroundColor Cyan
    & $gh repo create $full --public --description "Guitar Pedalboard: browser guitar multi-effects with tuner, EQ and tap tempo" | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "could not create $full" -ForegroundColor Red; exit 1 }
}

function Get-GitBlobSha([byte[]]$bytes) {
    $header = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha1 = [Security.Cryptography.SHA1]::Create()
    return (($sha1.ComputeHash([byte[]]($header + $bytes)) | ForEach-Object { $_.ToString("x2") }) -join "")
}

function Publish-File([string]$path, [byte[]]$bytes) {
    $remoteSha = & $gh api "repos/$full/contents/$path" --jq .sha 2> $null
    if ($LASTEXITCODE -eq 0 -and $remoteSha -and $remoteSha.Trim() -eq (Get-GitBlobSha $bytes)) {
        Write-Host "  $path unchanged"
        return
    }
    $body = @{ message = "Update $path"; content = [Convert]::ToBase64String($bytes) }
    if ($LASTEXITCODE -eq 0 -and $remoteSha) { $body.sha = $remoteSha.Trim() }
    # Windows PowerShell mangles large strings piped to programs, so hand the JSON over as a file
    $json = [IO.Path]::GetTempFileName()
    [IO.File]::WriteAllText($json, ($body | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding $false))
    & $gh api -X PUT "repos/$full/contents/$path" --input $json --silent
    $ok = $LASTEXITCODE -eq 0
    Remove-Item $json
    if (-not $ok) { Write-Host "upload of $path failed" -ForegroundColor Red; exit 1 }
    Write-Host "  uploaded $path"
}

Write-Host "== Uploading" -ForegroundColor Cyan
$dist = Join-Path $PSScriptRoot "dist"
Publish-File "index.html" ([IO.File]::ReadAllBytes((Join-Path $dist "index.html")))
Publish-File "worklet.js" ([IO.File]::ReadAllBytes((Join-Path $dist "worklet.js")))
Publish-File ".nojekyll" ([byte[]]@())

# the repository's front page: README with its screenshots
$github = Join-Path $PSScriptRoot "github"
Publish-File "README.md" ([IO.File]::ReadAllBytes((Join-Path $github "README.md")))
foreach ($image in Get-ChildItem (Join-Path $github "images") -Filter *.png) {
    Publish-File "images/$($image.Name)" ([IO.File]::ReadAllBytes($image.FullName))
}

& $gh api "repos/$full/pages" --silent 2> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "== Turning on GitHub Pages" -ForegroundColor Cyan
    $branch = (& $gh api "repos/$full" --jq .default_branch).Trim()
    & $gh api -X POST "repos/$full/pages" -f "source[branch]=$branch" -f "source[path]=/" --silent
    if ($LASTEXITCODE -ne 0) { Write-Host "could not turn on GitHub Pages" -ForegroundColor Red; exit 1 }
}

Write-Host "== Waiting for GitHub Pages to publish" -ForegroundColor Cyan
$status = ""
for ($i = 0; $i -lt 60 -and $status -ne "built"; $i++) {
    Start-Sleep -Seconds 5
    $status = (& $gh api "repos/$full/pages/builds/latest" --jq .status 2> $null)
    if ($status) { $status = $status.Trim() }
}
$url = (& $gh api "repos/$full/pages" --jq .html_url).Trim()
Write-Host ""
Write-Host "Published ($status): $url" -ForegroundColor Green
