# Every method in a title assembly that references a given member or string.
param(
  [Parameter(Mandatory=$true)][string]$Assembly,
  [Parameter(Mandatory=$true)][string]$Pattern,
  [string]$TitleDir = "F:\Xenia-Netplay2\build\bin\Windows\Release\xna_titles\6FD9AF209B36E1DA0031DC265AC11BFA0E62B81958"
)
$root = "F:\Xenia-Netplay2\build\bin\Windows\Release\xna"
Add-Type -Path (Join-Path $root "Mono.Cecil.dll")
$path = Join-Path $TitleDir $Assembly
$resolver = New-Object Mono.Cecil.DefaultAssemblyResolver
$resolver.AddSearchDirectory($TitleDir)
$resolver.AddSearchDirectory((Join-Path $root "console"))
$rp = New-Object Mono.Cecil.ReaderParameters
$rp.AssemblyResolver = $resolver
$module = [Mono.Cecil.ModuleDefinition]::ReadModule($path, $rp)
foreach ($t in $module.GetTypes()) {
  foreach ($m in $t.Methods) {
    if (-not $m.HasBody) { continue }
    foreach ($i in $m.Body.Instructions) {
      if ($i.Operand -and $i.Operand.ToString() -match $Pattern) {
        Write-Output ("{0}::{1}  ->  {2}" -f $t.FullName, $m.Name, $i.Operand)
      }
    }
  }
}
