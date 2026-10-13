# build_msvc.ps1 <out-dir>
#
# Build the C and C++ corpus with MSVC (cl/link from a Visual Studio
# developer shell) into PDBs, freestanding like corpus/build.sh. Used by
# CI only: the PDBs contain machine paths and stay build artifacts.
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$common = @('/nologo', '/c', '/Z7', '/GS-', '/Zl', '/Gs1000000')
function Build($name, $lang, $opt, $sources) {
    $objdir = Join-Path $Out "$name.obj"
    New-Item -ItemType Directory -Force -Path $objdir | Out-Null
    $objs = @()
    foreach ($src in $sources) {
        $obj = Join-Path $objdir ((Split-Path -Leaf $src) + '.obj')
        $flags = $common + @($opt)
        if ($lang -eq 'cxx') { $flags += @('/std:c++17', '/GR-', '/EHs-c-', '/Zc:threadSafeInit-', '/TP') }
        else { $flags += @('/std:c11', '/TC') }
        & cl.exe @flags (Join-Path $here $src) "/Fo$obj"
        if ($LASTEXITCODE) { throw "cl failed: $src" }
        $objs += $obj
    }
    & link.exe /nologo /DEBUG:FULL "/PDB:$Out\$name.pdb" "/OUT:$Out\$name.exe" /ENTRY:start `
        /SUBSYSTEM:CONSOLE /NODEFAULTLIB /Brepro /INCREMENTAL:NO @objs
    if ($LASTEXITCODE) { throw "link failed: $name" }
}
Build 'msvc_c_basic' 'c' '/Od' @('c_basic\main.c', 'c_basic\util.c')
Build 'msvc_c_basic_O2' 'c' '/O2' @('c_basic\main.c', 'c_basic\util.c')
Build 'msvc_cpp_classes' 'cxx' '/Od' @('cpp_classes\main.cpp', 'cpp_classes\shapes.cpp', 'cpp_classes\templates.cpp')
Build 'msvc_cpp_classes_O2' 'cxx' '/O2' @('cpp_classes\main.cpp', 'cpp_classes\shapes.cpp', 'cpp_classes\templates.cpp')
Write-Output "MSVC corpus built in $Out"
