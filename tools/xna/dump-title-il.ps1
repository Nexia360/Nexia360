# Same as dump-il.ps1 but for the title's own assemblies, which live in the
# unpacked package rather than the console runtime. Reading the GAME is how to
# see the next several walls at once instead of one per build.
param(
  [Parameter(Mandatory=$true)][string]$Assembly,
  [Parameter(Mandatory=$true)][string]$Type,
  [string]$Method = "",
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
  if ($t.Name -ne $Type -and $t.FullName -ne $Type) { continue }
  foreach ($m in $t.Methods) {
    if ($Method -and $m.Name -ne $Method) { continue }
    Write-Output ("=== " + $t.FullName + "::" + $m.Name)
    if (-not $m.HasBody) { Write-Output "    <no body>"; continue }
    foreach ($i in $m.Body.Instructions) { Write-Output ("    " + $i.ToString()) }
  }
}
