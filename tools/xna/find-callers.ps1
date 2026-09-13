# Which methods call a given method, anywhere in a console assembly.
param(
  [Parameter(Mandatory=$true)][string]$Assembly,
  [Parameter(Mandatory=$true)][string]$Name
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
  foreach ($m in $t.Methods) {
    if (-not $m.HasBody) { continue }
    foreach ($i in $m.Body.Instructions) {
      if ($i.Operand -and $i.Operand.ToString() -match $Name) {
        Write-Output ("{0}::{1}   {2}" -f $t.FullName, $m.Name, $i.Operand)
      }
    }
  }
}
