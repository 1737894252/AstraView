Option Explicit
Dim fso, scriptDir, arguments, shell
Set fso = CreateObject("Scripting.FileSystemObject")
scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
arguments = "-NoProfile -ExecutionPolicy Bypass -File """ & scriptDir & "\install.ps1"""
Set shell = CreateObject("Shell.Application")
shell.ShellExecute "powershell.exe", arguments, "", "runas", 1
