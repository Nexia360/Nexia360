# Embedded resources in a console assembly, with the first bytes of each. The
# built-in effects (BasicEffect, AlphaTestEffect and friends) ship their
# compiled shader code this way rather than in an .xnb.
param([Parameter(Mandatory=$true)][string]$Assembly)
$root = "F:\Xenia-Netplay2\build\bin\Windows\Release\xna"
Add-Type -Path (Join-Path $root "Mono.Cecil.dll")
$module = [Mono.Cecil.ModuleDefinition]::ReadModule((Join-Path $root "console\$Assembly"))
foreach ($r in $module.Resources) {
  if ($r -isnot [Mono.Cecil.EmbeddedResource]) { Write-Output ($r.Name + "  (not embedded)"); continue }
  $d = $r.GetResourceData()
  $head = ""
  for ($i = 0; $i -lt [Math]::Min(16, $d.Length); $i++) { $head += "{0:x2} " -f $d[$i] }
  Write-Output ("{0,-46} {1,8} bytes  {2}" -f $r.Name, $d.Length, $head)
}
