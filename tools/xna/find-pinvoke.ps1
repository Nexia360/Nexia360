# Lists every P/Invoke in a console assembly with its full managed signature.
# The .def only carries names; this is where the arguments and return type come
# from. Optional -Match filters on the entry point name.
param(
  [Parameter(Mandatory=$true)][string]$Assembly,
  [string]$Match = ""
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
    if (-not $m.PInvokeInfo) { continue }
    $entry = $m.PInvokeInfo.EntryPoint
    if ($Match -and $entry -notmatch $Match) { continue }
    $args = ($m.Parameters | ForEach-Object {
      $p = $_.ParameterType.FullName
      if ($_.IsOut) { "out $p" } elseif ($p.EndsWith("&")) { "ref $p" } else { $p }
    }) -join ", "
    Write-Output ("{0}!{1}  ->  {2} {3}({4})" -f $m.PInvokeInfo.Module.Name, $entry,
                  $m.ReturnType.FullName, $m.Name, $args)
  }
}
