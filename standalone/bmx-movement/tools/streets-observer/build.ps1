param([string]$GameRoot='D:\SteamLibrary\steamapps\common\BMX Streets')
$ErrorActionPreference='Stop'
$taskProjectRoot=$PSScriptRoot
$taskOutput=Join-Path $taskProjectRoot 'bin'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskRuntime='C:\Program Files\dotnet\shared\Microsoft.NETCore.App\6.0.36'
if(-not(Test-Path -LiteralPath $taskRuntime)){throw 'Existing .NET6 runtime reference set missing'}
Add-Type -Path (Join-Path $PSHOME 'Microsoft.CodeAnalysis.dll')
Add-Type -Path (Join-Path $PSHOME 'Microsoft.CodeAnalysis.CSharp.dll')
$taskReferences=[System.Collections.Generic.List[Microsoft.CodeAnalysis.MetadataReference]]::new()
$taskReferencePaths=@(Get-ChildItem -LiteralPath $taskRuntime -Filter '*.dll' -File | ForEach-Object FullName)
$taskReferencePaths+=@(Get-ChildItem -LiteralPath (Join-Path $GameRoot 'MelonLoader\Il2CppAssemblies') -Filter '*.dll' -File | ForEach-Object FullName)
$taskReferencePaths+=@('MelonLoader.dll','Il2CppInterop.Runtime.dll','Il2CppInterop.Common.dll' | ForEach-Object { Join-Path (Join-Path $GameRoot 'MelonLoader\net6') $_ })
foreach($taskPath in $taskReferencePaths){
    $taskPeStream=[System.IO.File]::OpenRead($taskPath)
    $taskPeReader=[System.Reflection.PortableExecutable.PEReader]::new($taskPeStream)
    try{if($taskPeReader.HasMetadata){$taskReferences.Add([Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile($taskPath))}}
    finally{$taskPeReader.Dispose();$taskPeStream.Dispose()}
}
$taskSyntaxList=[System.Collections.Generic.List[Microsoft.CodeAnalysis.SyntaxTree]]::new()
Get-ChildItem -LiteralPath $taskProjectRoot -Filter '*.cs' -File | Sort-Object Name | ForEach-Object { $taskSyntaxList.Add([Microsoft.CodeAnalysis.CSharp.CSharpSyntaxTree]::ParseText([System.IO.File]::ReadAllText($_.FullName))) }
$taskOptions=[Microsoft.CodeAnalysis.CSharp.CSharpCompilationOptions]::new([Microsoft.CodeAnalysis.OutputKind]::DynamicallyLinkedLibrary).WithOptimizationLevel([Microsoft.CodeAnalysis.OptimizationLevel]::Release)
$taskCompilation=[Microsoft.CodeAnalysis.CSharp.CSharpCompilation]::Create('BmxMovementObserver',$taskSyntaxList.ToArray(),$taskReferences,$taskOptions)
$taskDll=Join-Path $taskOutput 'BmxMovementObserver.dll'
$taskStream=[System.IO.File]::Open($taskDll,[System.IO.FileMode]::Create)
try{$taskResult=$taskCompilation.Emit($taskStream)}finally{$taskStream.Dispose()}
$taskResult.Diagnostics | ForEach-Object { $_.ToString() }
if(-not $taskResult.Success){throw 'Observer compilation failed'}
[pscustomobject]@{Assembly=$taskDll;Bytes=(Get-Item -LiteralPath $taskDll).Length;Sha256=(Get-FileHash -LiteralPath $taskDll -Algorithm SHA256).Hash;SourceOnly=$true;Deployed=$false}|ConvertTo-Json
