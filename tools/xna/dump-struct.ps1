# Field layout of a type in a console assembly - the shape a native shim has to
# declare in C++ to read what XNA passes it.
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
  Write-Output ("=== " + $t.FullName + "  (layout " + $t.Attributes + ", pack " + $t.PackingSize + ", size " + $t.ClassSize + ")")
  foreach ($f in $t.Fields) {
    if ($f.IsStatic) { continue }
    Write-Output ("    +{0,-4} {1} {2}" -f $f.Offset, $f.FieldType.FullName, $f.Name)
  }
}
