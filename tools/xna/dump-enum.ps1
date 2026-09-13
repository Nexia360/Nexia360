# The values of an enum in a console assembly. HLCBPacketType is the whole
# vocabulary of the command stream XNA hands to D3D_Device_ReceivePackets.
param(
  [Parameter(Mandatory=$true)][string]$Assembly,
  [Parameter(Mandatory=$true)][string]$Type
)
$root = Join-Path $PSScriptRoot "..\..\build\bin\Windows\Release\xna"
Add-Type -Path (Join-Path $root "Mono.Cecil.dll")
$path = Join-Path $root "console\$Assembly"
$resolver = New-Object Mono.Cecil.DefaultAssemblyResolver
$resolver.AddSearchDirectory((Split-Path $path))
$rp = New-Object Mono.Cecil.ReaderParameters
$rp.AssemblyResolver = $resolver
$module = [Mono.Cecil.ModuleDefinition]::ReadModule($path, $rp)
foreach ($t in $module.GetTypes()) {
  if ($t.Name -ne $Type -and $t.FullName -ne $Type) { continue }
  Write-Output ("=== " + $t.FullName)
  foreach ($f in $t.Fields) {
    if (-not $f.HasConstant) { continue }
    Write-Output ("    {0,-6} {1}" -f $f.Constant, $f.Name)
  }
}
