# build_pgo.ps1 <out-dir>
#
# Build corpus/pgo/pgo.c with MSVC profile-guided optimization: an
# instrumented build, a training run, then the optimized build whose PDB
# (pgo.pdb) has separated code (S_SEPCODE) and call counts. CI only, from
# an x64 developer shell; the C runtime is linked.
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Push-Location $Out
try {
    New-Item -ItemType Directory -Force -Path train | Out-Null
    & cl.exe /nologo /c /O2 /GL /Z7 /GS- (Join-Path $here 'pgo.c') /Fotrain\pgo.obj
    if ($LASTEXITCODE) { throw "cl failed" }
    & link.exe /nologo /DEBUG:FULL /LTCG /GENPROFILE /PDB:train\pgo_instr.pdb /OUT:train\pgo_instr.exe /PGD:pgo.pgd train\pgo.obj
    if ($LASTEXITCODE) { throw "link /GENPROFILE failed" }
    $vcbin = Split-Path -Parent (Get-Command cl.exe).Source
    $env:PATH = "$vcbin;$env:PATH"
    & .\train\pgo_instr.exe 20000
    if ($LASTEXITCODE) { throw "training run failed" }
    & link.exe /nologo /DEBUG:FULL /LTCG /USEPROFILE /PDB:pgo.pdb /OUT:pgo.exe /PGD:pgo.pgd train\pgo.obj
    if ($LASTEXITCODE) { throw "link /USEPROFILE failed" }
} finally {
    Pop-Location
}
Write-Output "PGO corpus built in $Out"
