# VB Decompiler Plugin API

This documentation describes the interface for creating plugins for VB Decompiler. The plugin system allows you to analyze, modify, or extend the decompiled code and internal structures of the application via a shared library (DLL).


## Architecture Overview

The plugin system operates on a callback model:

- VB Decompiler loads your DLL plugin.
- It calls your exported functions to retrieve the plugin name and author info.
- It calls  VBDecompilerPluginLoad , passing a pointer to its internal  Callback Engine .
- Your plugin interacts with the host by calling this Engine function using specific Action IDs ( vlType constants).

##  Exported Functions

Your DLL must export the following  stdcall functions:

#### VBDecompilerPluginName

Called by the host to retrieve the display name of your plugin.

```
void __stdcall VBDecompilerPluginName(  
HWND VBDecompilerHWND, // Handle to main window  
HWND RichTextBoxHWND,  // Handle to the code editor window  
char* Buffer,          // Target buffer (100 bytes, ANSI)  
int Reserved           // Reserved 
);
```

Usage: Copy a standard ASCII string (max 100 chars) with your plugin name into  Buffer .

#### VBDecompilerPluginAuthor

Called to retrieve author contact information.

```
void __stdcall VBDecompilerPluginAuthor(  
HWND VBDecompilerHWND,  
HWND RichTextBoxHWND,  
char* Buffer,          // Target buffer (ANSI)  
int Reserved 
);
```

Usage: Copy a string like  "Author Name, email@domain.com" into  Buffer .

#### VBDecompilerPluginLoad

The main entry point where your plugin logic is executed.

```
void __stdcall VBDecompilerPluginLoad(  
HWND VBDecompilerHWND,  
HWND RichTextBoxHWND,  
char* Buffer,          // Reserved/Buffer  
void* PluginEngine     // <--- Pointer to the Callback Function 
);
```

*Usage:* Store the  PluginEngine pointer. You will use it to invoke  GetValue /  SetValue commands (see "Callback Engine" below).

##  The Callback Engine

Communication with VB Decompiler is handled via a single callback function pointer provided in  PluginEngine .

 Function Signature:

```
typedef wchar_t* (__stdcall *TVBDPluginEngine)(  
int vlType,       // Command ID (Action)  
int vlNumber,     // Primary Index (Form/Module ID)  
int vlFnNumber,   // Secondary Index (Function ID)  
char* vlNewValue  // Value to set (ANSI string) or NULL for Get commands 
);
```

Note: The engine accepts ANSI strings (char*) for inputs but returns Unicode strings (wchar_t*) for results.

##  Command Reference (vlType)

The following constants are used as the  vlType argument to perform specific actions.

##### Global Project Commands

Operations related to the project file or global settings.  vlNumber and  vlFnNumber are usually 0.

| ID   | Constant            | Description                                                  |
| ---- | ------------------- | ------------------------------------------------------------ |
| 1    | GetVBProject        | Get the full text of the decompiled project file.            |
| 2    | SetVBProject        | Replace the project file content.                            |
| 3    | GetFileName         | Get the full path of the file being analyzed. (v3.5+)        |
| 4    | IsNativeCompilation | Returns "1" if the target is compiled to Native Code, "0" for P-Code. (v3.5+) |
| 5    | ClearAllBuffers     | Clears internal decompiler structures. Use this if your plugin implements a full custom decompiler for a new language. (v3.9+) |
| 6    | GetCompiler         | Returns compiler type: 1=VB, 2=.NET, 3=Delphi, 4=Unknown. (v3.9+) |
| 7    | IsPacked            | Returns "1" if the file is packed/compressed, "0" otherwise. (v3.9+) |
| 56   | GetVBDecompilerPath | Returns the path to the VB Decompiler executable. (v3.5+)    |
| 100  | UpdateAll           | Commit changes and refresh the GUI.  Required after setting values to ensure the user sees changes. |

#### UI Control

Control elements of the VB Decompiler interface.

| ID   | Constant                  | Description                                                  |
| ---- | ------------------------- | ------------------------------------------------------------ |
| 8    | SetStackCheckBoxValue     | Set  Parse stack parameters checkbox state. "1" = Checked, "0" = Unchecked. (v3.9+) |
| 9    | SetAnalyzerCheckBoxValue  | Set  Procedure analyzer and optimizer checkbox state. "1" = Checked, "0" = Unchecked. (v3.9+) |
| 50   | GetActiveText             | Get text currently displayed in the active code window.      |
| 51   | SetActiveText             | Set text in the active code window.                          |
| 52   | GetActiveDisasmText       | Get the raw disassembly text of the disassembler window. (v9.4+) |
| 53   | SetActiveDisasmText       | Set the raw disassembly text to the disassembler window. (v9.4+) |
| 54   | SetActiveTextLine         | Scroll or jump to a specific line number (passed as string in vlNewValue). |
| 55   | GetActiveModuleCoordinats | Get IDs of the active view. Likely returns format "ModuleIndex,FunctionIndex". (v3.5+) |
| 58   | SetStatusBarText          | Display a custom message in the application status bar. (v3.5+) |

#### Forms

Operations on VB Forms.  vlNumber : Form Index (0-based).

| ID   | Constant       | Description                                   |
| ---- | -------------- | --------------------------------------------- |
| 10   | GetVBFormName  | Get the name of the form.                     |
| 11   | SetVBFormName  | Rename the form.                              |
| 12   | GetVBForm      | Get the decompiled properties of the form.    |
| 13   | SetVBForm      | Set the properties of the form.               |
| 14   | GetVBFormCount | Returns total number of forms in the project. |

#### Modules

Operations on Standard Modules (.bas) or Class Modules (.cls).  vlNumber : Module Index (0-based).

| ID   | Constant                  | Description                                           |
| ---- | ------------------------- | ----------------------------------------------------- |
| 30   | GetModuleName             | Get the name of the module.                           |
| 31   | SetModuleName             | Rename the module.                                    |
| 32   | GetModule                 | Get the decompiled declarations header of the module. |
| 33   | SetModule                 | Set the decompiled declarations header of the module. |
| 34   | GetModuleStringReferences | Get list of string references used in this module.    |
| 35   | SetModuleStringReferences | Set string references.                                |
| 36   | GetModuleCount            | Returns total number of modules.                      |

#### Procedures / Functions

Operations on specific functions within a module.
 vlNumber : Module Index.
 vlFnNumber : Function Index (0-based) within that module.

| ID   | Constant                 | Description                                                  |
| ---- | ------------------------ | ------------------------------------------------------------ |
| 20   | GetSubMain               | Get the code for the entry point (Sub Main). (Index args ignored). |
| 21   | SetSubMain               | Set the code for the entry point.                            |
| 40   | GetModuleFunctionName    | Get the name of a specific function.                         |
| 41   | SetModuleFunctionName    | Rename the function.                                         |
| 42   | GetModuleFunctionAddress | Get the memory address (VA) of the function.                 |
| 43   | SetModuleFunctionAddress | Set the function address.                                    |
| 44   | GetModuleFunction        | Get the decompiled code of the function.                     |
| 45   | SetModuleFunction        | Set the code of the function.                                |
| 46   | GetModuleFunctionStrRef  | Get string references specific to this function.             |
| 47   | SetModuleFunctionStrRef  | Set string references for this function.                     |
| 48   | GetModuleFunctionCount   | Returns number of functions in the specified module.         |
| 57   | GetModuleFunctionCode    | Get function code in "Fast Decompilation" mode (if this function is not processed at this time). (v3.5+) |
| 70   | GetModuleFunctionDisasm  | Get the disassembly (ASM) of the function. (v9.4+)           |
| 71   | SetModuleFunctionDisasm  | Set the disassembly of the function. (v9.4+)                 |

#### Resources (FRX)

Access to binary form resources (icons/images).  vlNumber : Icon Index.

| ID   | Constant         | Description                                           |
| ---- | ---------------- | ----------------------------------------------------- |
| 60   | GetFrxIconCount  | Returns total number of icons/images in FRX. (v5.0+)  |
| 61   | GetFrxIconOffset | Get the file offset for the specific icon. (v5.0+)    |
| 62   | GetFrxIconSize   | Get the size (in bytes) of the specific icon. (v5.0+) |

