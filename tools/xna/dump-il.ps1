# Dumps the IL of methods in a console XNA assembly, using the Mono.Cecil that
# already ships beside the managed host.
#
# Reading the CALLER is the only reliable way to give a console entry point the
# right meaning: the P/Invoke signature says what the arguments are, never what
# the return value means. Two shims were written wrong from the signature alone
# before this existed - GetSupportedDisplayMode ends its loop by FAILING, so a
# stub returning success enumerated until memory ran out.
#
#   dump-il.ps1 MXF.dlx SoundEffect ..cctor
#   dump-il.ps1 MXF.dlx UserAsyncDispatcher            # every method on a type
param(
  [Parameter(Mandatory=$true)][string]$Assembly,
  [Parameter(Mandatory=$true)][string]$Type,
  [string]$Method = ""
)

$root = Join-Path $PSScriptRoot "..\..\build\bin\Windows\Release\xna"
$cecil = Join-Path $root "Mono.Cecil.dll"
if (-not (Test-Path $cecil)) { throw "Mono.Cecil not found at $cecil" }
Add-Type -Path $cecil

$path = $Assembly
if (-not (Test-Path $path)) { $path = Join-Path $root "console\$Assembly" }
if (-not (Test-Path $path)) { throw "assembly not found: $Assembly" }

# The console assemblies reference each other and the console BCL, so the
# resolver has to look beside them or every type reference fails to resolve.
$resolver = New-Object Mono.Cecil.DefaultAssemblyResolver
$resolver.AddSearchDirectory((Split-Path $path))
$rp = New-Object Mono.Cecil.ReaderParameters
$rp.AssemblyResolver = $resolver
$module = [Mono.Cecil.ModuleDefinition]::ReadModule($path, $rp)

$types = $module.GetTypes() | Where-Object { $_.Name -eq $Type -or $_.FullName -eq $Type }
if (-not $types) { throw "type not found: $Type" }

foreach ($t in $types) {
  foreach ($m in $t.Methods) {
    if ($Method -and $m.Name -ne $Method) { continue }
    Write-Output ""
    Write-Output ("=== " + $t.FullName + "::" + $m.Name + " " + $m.ToString())
    if ($m.PInvokeInfo) {
      Write-Output ("    [pinvoke] " + $m.PInvokeInfo.Module.Name + "!" + $m.PInvokeInfo.EntryPoint)
    }
    if (-not $m.HasBody) { Write-Output "    <no body>"; continue }
    foreach ($i in $m.Body.Instructions) { Write-Output ("    " + $i.ToString()) }
  }
}
