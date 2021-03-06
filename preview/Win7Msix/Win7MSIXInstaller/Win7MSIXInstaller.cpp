// Win7MSIXInstaller.cpp :
// The main entry point for Win7MSIXInstaller.exe. This application is
// a working preview for the MSIX/APPX installer for Windows 7
#include "MSIXWindows.hpp"
#include <shlobj_core.h>
#include <CommCtrl.h>

#include <string>
#include <map>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <utility>
#include <iomanip>
#include <functional>
#include <thread>

#include "AppxPackaging.hpp"
#include "RegUtil.h"
#include "Logger.h"
#include "GeneralUtil.h"
#include "FootprintFiles.h"
#include "Extractor.h"
#include "Uninstall.h"
#include "FilePaths.h"
#include "InstallUI.h"
#include <cstdio>
#include <experimental/filesystem> // C++-standard header file name
#include <filesystem> // Microsoft-specific implementation header file name

// MSIXWindows.hpp define NOMINMAX because we want to use std::min/std::max from <algorithm>
// GdiPlus.h requires a definiton for min and max. Use std namespace *BEFORE* including it.
using namespace std;
#include <GdiPlus.h>

// Global variables
static const std::wstring g_windowClass = L"DesktopApp"; // The main window class name.
static const std::wstring g_title = L"MSIX Installer"; // The string that appears in the application's title bar.
static const std::wstring g_cancelMessage = L"Cancel";
static const std::wstring g_closeMessage = L"Close";
static std::wstring g_completeTitle = L"Installation Completed";
static std::wstring g_uninstallCompleteTitle = L"Uninstallation Completed";
static std::wstring g_completeMessage = L"You may close this installation window";
static std::wstring g_messageText = L"";
static std::wstring g_displayText = L"";
static int g_numPayloadFiles = 0;
static std::wstring g_pathToUninstall; // The path to a previous version if the application being installed is a newer version of an existing app
static std::wstring g_packageFullName; // The packageFullName is the currently installed application

//
// Describes an option to a command that the user may specify used for the command line tool
//
struct Option
{
    using CBF = std::function<bool(const std::string& value)>;

    Option(bool param, const std::string& help, CBF callback) : Help(help), Callback(callback), TakesParameter(param)
    {}

    bool        TakesParameter;
    std::string Name;
    std::string Help;
    CBF         Callback;
};

static State g_state;
static std::map<std::string, Option> g_options =
{
    {
        "-p", Option(true, "REQUIRED, specify input package name.",
        [&](const std::string& name) { return g_state.SetPackageName(name); })
    },
    {
        "-sv", Option(false, "Skips signature validation.  By default signature validation is enabled.",
        [&](const std::string&) { return g_state.AllowSignatureOriginUnknown(); })
    },
    {
        "-ss", Option(false, "Skips enforcement of signed packages.  By default packages must be signed.",
        [&](const std::string&)
        {
            g_footprintFilesType[2].isRequired = false;
            return g_state.SkipSignature();
        })
    },
    {
        "-x", Option(true, "Uninstalls the specified uninstaller xml.",
        [&](const std::string& uninstallXml) { return g_state.SetUninstallXml(uninstallXml); })
    },
    {
        "-?", Option(false, "Displays this help text.",
        [&](const std::string&) { return false; })
    }
};

//
// Displays contextual formatted help to the user used for command line tool
//
// Parameters:
// toolName - A character specifiying which tool to use
// options    - A map containing all the options
//
int Help(char* toolName)
{
    std::cout << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "------" << std::endl;
    std::cout << "\t" << toolName << " -p <package> [options] " << std::endl;
    std::cout << std::endl;
    std::cout << "Description:" << std::endl;
    std::cout << "------------" << std::endl;
    std::cout << "\tExtracts all files within an app package at the input <package> name to the" << std::endl;
    std::cout << "\tspecified output <directory>.  The output has the same directory structure " << std::endl;
    std::cout << "\tas the package." << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "--------" << std::endl;

    for (const auto& option : g_options)
    {
        std::cout << "\t" << std::left << std::setfill(' ') << std::setw(5) <<
            option.first << ": " << option.second.Help << std::endl;
    }
    return 0;
}

//
// Displays an error text if the user provided underspecified input for the command lien tool
//
// Parameters:
// toolName -  A character specifiying which tool to use
//
void Error(char* toolName)
{
    std::cout << toolName << ": error : Missing required options.  Use '-?' for more details." << std::endl;
}

HRESULT RemovePackage()
{
	auto xmlFilePathWSTR = g_state.m_uninstallXml;

	// Initializes the XML reader object
	TrackerXML myXML(xmlFilePathWSTR);

	// Goes through all the files in the XML and removes them
	FileEnumerator xmlFileEnum = myXML.GetFileEnumerator();
	while (xmlFileEnum.HasNext())
	{
		std::wcout << xmlFileEnum.GetCurrent() << std::endl;
		std::wstring fileToRemove = xmlFileEnum.GetCurrent();
		std::experimental::filesystem::remove(fileToRemove);
		xmlFileEnum.MoveNext();
	}
	std::experimental::filesystem::remove(xmlFileEnum.GetCurrent());

	// Goes through all the directories created in the XML and removes them
	DirectoryEnumerator xmlDirEnum = myXML.GetDirectoryEnumerator();
	while (xmlDirEnum.HasNext())
	{
		std::wcout << xmlDirEnum.GetCurrent() << std::endl;
		std::wstring dirToRemove = xmlDirEnum.GetCurrent();
		std::experimental::filesystem::remove(dirToRemove);
		xmlDirEnum.MoveNext();
	}
	std::experimental::filesystem::remove(xmlDirEnum.GetCurrent());

	// Deletes the package in the WindowsApps folder
	std::wstring packagePath = L"C:\\Program Files (x86)\\WindowsApps\\" + myXML.GetPackageName();
	std::experimental::filesystem::remove_all(packagePath);

	// Deletes the start menu shortcut
	std::wstring startMenuShortcutPath = L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\" + myXML.GetDisplayName() + L".lnk";
	std::experimental::filesystem::remove(startMenuShortcutPath);

	// Deletes the registry keys added
	RegDeleteAll(&myXML);

	// Deletes the XML file itself
	std::experimental::filesystem::remove(xmlFilePathWSTR);

    ChangeButtonText(g_closeMessage);
    g_installed = true;
    g_displayCompleteText = true;

	return S_OK;
}

//
// Creates a cross-plat app package.
//
// Parameters:
//   state - Contains the information for package name and validation options.
//   reader - On success, receives the created instance of IAppxPackageReader.
//
HRESULT GetPackageReader(State& state, IAppxPackageReader** package)
{
    ComPtr<IAppxFactory> appxFactory;
    ComPtr<IStream> inputStream;
    *package = nullptr;
    RETURN_IF_FAILED(CreateStreamOnFileUTF16(state.packageName.c_str(), true, &inputStream));

    // On Win32 platforms CoCreateAppxFactory defaults to CoTaskMemAlloc/CoTaskMemFree
    // On non-Win32 platforms CoCreateAppxFactory will return 0x80070032 (e.g. HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))
    // So on all platforms, it's always safe to call CoCreateAppxFactoryWithHeap, just be sure to bring your own heap!
    RETURN_IF_FAILED(CoCreateAppxFactoryWithHeap(
        MyAllocate,
        MyFree,
        state.validationOptions,
        &appxFactory));

    // Create a new package reader using the factory.  For
    // simplicity, we don't verify the digital signature of the package.
    RETURN_IF_FAILED(appxFactory->CreatePackageReader(inputStream.Get(), package));
    return S_OK;
}

//
// Gets the stream of a file.
//
// Parameters:
//   package - The package reader for the app package.
//   name - Name of the file.
//   stream - The stream for the file.
//
HRESULT GetStreamFromFile(IAppxPackageReader* package, LPCWCHAR name, IStream** stream)
{
    *stream = nullptr;

    ComPtr<IAppxFilesEnumerator> files;
    RETURN_IF_FAILED(package->GetPayloadFiles(&files));

    BOOL hasCurrent = FALSE;
    RETURN_IF_FAILED(files->GetHasCurrent(&hasCurrent));
    while (hasCurrent)
    {
        ComPtr<IAppxFile> file;
        RETURN_IF_FAILED(files->GetCurrent(&file));
        Text<WCHAR> fileName;
        file->GetName(&fileName);
        if (wcscmp(fileName.Get(), name) == 0)
        {
            RETURN_IF_FAILED(file->GetStream(stream));
            return S_OK;
        }
        RETURN_IF_FAILED(files->MoveNext(&hasCurrent));
    }
    return S_OK;
}

// Uses the Shell's IShellLink and IPersistFile interfaces
// to create and store a shortcut to the specified object.
//
// Parameters:
// lpszPathObj  - Address of a buffer that contains the path of the object,
//                including the file name.
// lpszPathLink - Address of a buffer that contains the path where the
//                Shell link is to be stored, including the file name.
// lpszDesc     - Address of a buffer that contains a description of the
//                Shell link, stored in the Comment field of the link
//                properties.
//
HRESULT CreateLink(LPCWSTR lpszPathObj, LPCSTR lpszPathLink, LPCWSTR lpszDesc)
{
    ComPtr<IShellLink> shellLink;

    // Get a pointer to the IShellLink interface. It is assumed that CoInitialize
    // has already been called.
    RETURN_IF_FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, reinterpret_cast<LPVOID*>(&shellLink)));

    // Set the path to the shortcut target and add the description.
    RETURN_IF_FAILED(shellLink->SetPath(lpszPathObj));
    RETURN_IF_FAILED(shellLink->SetDescription(lpszDesc));

    // Query IShellLink for the IPersistFile interface, used for saving the shortcut in persistent storage.
    ComPtr<IPersistFile> persistFile;
    RETURN_IF_FAILED(shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<LPVOID*>(&persistFile)));
    WCHAR wsz[MAX_PATH];

    // Ensure that the string is Unicode.
    MultiByteToWideChar(CP_ACP, 0, lpszPathLink, -1, wsz, MAX_PATH);

    // Save the link by calling IPersistFile::Save.
    RETURN_IF_FAILED(persistFile->Save(wsz, TRUE));
    return S_OK;
}

// Helper to convert version number to a version number string
std::wstring ConvertVersionToString(UINT64 version)
{
    return std::to_wstring((version >> 0x30) & 0xFFFF) + L"."
        + std::to_wstring((version >> 0x20) & 0xFFFF) + L"."
        + std::to_wstring((version >> 0x10) & 0xFFFF) + L"."
        + std::to_wstring((version) & 0xFFFF);
}

//
// Parses argc/argv input via commands into state, obtains relevant information from the manifest
// then extracts the files from the VFS, extracts the entire package into C:\Program Files (x86)\WindowsApps,
// places the application in the start menu and then registers the application in add and remove programs.
//
// Parameters:
// state - The state of the package
// argc  - An integer representing the number of command line arguments
// argv  - An array of the command line arugments
//
HRESULT ParseAndRun(HWND hWnd)
{
    // We should had already parse the input here. Verify is correct.
    if (g_state.packageName.empty())
    {
        return E_FAIL;
    }

    ComPtr<IAppxPackageReader> package;
    RETURN_IF_FAILED(GetPackageReader(g_state, &package));

    // Initiates COM pointers to read the manifest
    ComPtr<IAppxManifestReader> manifest;
    RETURN_IF_FAILED(package->GetManifest(&manifest));
    ComPtr<IMsixDocumentElement> domElement;
    RETURN_IF_FAILED(manifest->QueryInterface(UuidOfImpl<IMsixDocumentElement>::iid, reinterpret_cast<void**>(&domElement)));
    ComPtr<IMsixElement> element;
    RETURN_IF_FAILED(domElement->GetDocumentElement(&element));
    ComPtr<IMsixElementEnumerator> veElementEnum;
    ComPtr<IMsixElementEnumerator> applicationElementEnum;
    BOOL hc = FALSE;

    // Obtains the Executable Path
    Text<wchar_t> executablePath;
    RETURN_IF_FAILED(element->GetElements(
        L"/*[local-name()='Package']/*[local-name()='Applications']/*[local-name()='Application']",
        &applicationElementEnum));
    RETURN_IF_FAILED(applicationElementEnum->GetHasCurrent(&hc));
    if (hc)
    {
        ComPtr<IMsixElement> applicationElement;
        RETURN_IF_FAILED(applicationElementEnum->GetCurrent(&applicationElement));
        RETURN_IF_FAILED(applicationElement->GetAttributeValue(L"Executable", &executablePath));
    }

    // Obtains the Display Name
    Text<wchar_t> displayName;
    RETURN_IF_FAILED(element->GetElements(
        L"/*[local-name()='Package']/*[local-name()='Applications']/*[local-name()='Application']/*[local-name()='VisualElements']",
        &veElementEnum));
    hc = FALSE;
    RETURN_IF_FAILED(veElementEnum->GetHasCurrent(&hc));
    if (hc)
    {
        ComPtr<IMsixElement> visualElementsElement;
        RETURN_IF_FAILED(veElementEnum->GetCurrent(&visualElementsElement));
        RETURN_IF_FAILED(visualElementsElement->GetAttributeValue(L"DisplayName", &displayName));
    }

    // Get the publisher, version and packageFullName
    ComPtr<IAppxManifestPackageId> manifestId;
    RETURN_IF_FAILED(manifest->GetPackageId(&manifestId));
    Text<WCHAR> publisher;
    RETURN_IF_FAILED(manifestId->GetPublisher(&publisher));
    UINT64 version;
    RETURN_IF_FAILED(manifestId->GetVersion(&version));
    Text<WCHAR> packageFullName;
    RETURN_IF_FAILED(manifestId->GetPackageFullName(&packageFullName));
    g_packageFullName = packageFullName.Get();

    // Designate where to place the uninstaller XML file
    CreateDirectory(L"C:\\Program Files (x86)\\Uninstallers", NULL);

    TrackerXML myXml;
    myXml.SetXMLName(L"C:\\Program Files (x86)\\Uninstallers", packageFullName.Get());
    myXml.SetDisplayName(displayName.Get());
    myXml.SetPackageName(packageFullName.Get());
    std::wstring localExecutablePath = GetExecutablePath(executablePath.Get(), packageFullName.Get());

    //Obtains the Extensions. Currently only supporting the FTA and protocol extension
    ComPtr<IMsixElementEnumerator> extensionEnum;
    Text<wchar_t> protocol;

    RETURN_IF_FAILED(element->GetElements(L"/*[local-name()='Package']/*[local-name()='Applications']/*[local-name()='Application']/*[local-name()='Extensions']/*[local-name()='Extension']", &extensionEnum));
    hc = FALSE;
    RETURN_IF_FAILED(extensionEnum->GetHasCurrent(&hc));
    while (hc)
    {
        ComPtr<IMsixElement> extensionElement;
        RETURN_IF_FAILED(extensionEnum->GetCurrent(&extensionElement));
        Text<wchar_t> extensionCategory;
        RETURN_IF_FAILED(extensionElement->GetAttributeValue(L"Category", &extensionCategory));

        if (wcscmp(extensionCategory.Get(), L"windows.fileTypeAssociation") == 0)
        {
            BOOL hc_fta;
            ComPtr<IMsixElementEnumerator> ftaEnum;
            RETURN_IF_FAILED(extensionElement->GetElements(L"/*[local-name()='FileTypeAssociations']", &ftaEnum));
            RETURN_IF_FAILED(ftaEnum->GetHasCurrent(&hc_fta));
            if (hc_fta)
            {
                ComPtr<IMsixElement> ftaElement;
                RETURN_IF_FAILED(ftaEnum->GetCurrent(&ftaElement));
                Text<wchar_t> ftaName;
                RETURN_IF_FAILED(ftaElement->GetAttributeValue(L"Name", &ftaName));

            }
        }
        else if (wcscmp(extensionCategory.Get(), L"windows.protocol") == 0)
        {
            BOOL hc_protocol;
            ComPtr<IMsixElementEnumerator> protocolEnum;
            RETURN_IF_FAILED(extensionElement->GetElements(L"/*[local-name()='Protocol']", &protocolEnum));
            RETURN_IF_FAILED(protocolEnum->GetHasCurrent(&hc_protocol));
            if (hc_protocol)
            {
                ComPtr<IMsixElement> protocolElement;
                RETURN_IF_FAILED(protocolEnum->GetCurrent(&protocolElement));
                Text<wchar_t> protocolName;
                RETURN_IF_FAILED(protocolElement->GetAttributeValue(L"Name", &protocolName));
                // Logic to write the code for the inserting into the registry
            }
        }
        RETURN_IF_FAILED(extensionEnum->MoveNext(&hc));
    }


    // Extracts package to WindowsApps\PackageFullName
    std::wstring packagePath = L"C:\\Program Files (x86)\\WindowsApps\\";
    CreateDirectory(packagePath.c_str(), NULL);
    packagePath.append(packageFullName.Get());
    CreateDirectory(packagePath.c_str(), NULL);
    RETURN_IF_FAILED(ExtractPackage(package.Get(), packagePath.c_str(), &myXml));

    //Places the application in the start menu
    std::wstring displayNameWString = std::wstring(displayName.Get());
    std::string displayNameString = utf16_to_utf8(displayNameWString);
    std::string filePath = "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\" + displayNameString + ".lnk";
    CreateLink(GetExecutablePath(executablePath.Get(), displayName.Get()).c_str(), filePath.c_str(), L"");

    //Registers application in Add/Remove Programs
    std::wstring regPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + displayNameWString + L"\\";
    RegWriteKeyValueSZ(HKEY_LOCAL_MACHINE, regPath.c_str(), L"DisplayName", displayName.Get(), &myXml);
    RegWriteKeyValueSZ(HKEY_LOCAL_MACHINE, regPath.c_str(), L"InstallLocation", &packagePath, &myXml);
    std::wstring packageFullNameWSTR = packageFullName.Get();
    std::wstring uninstallXmlPath = L"\"C:\\Program Files (x86)\\Uninstallers\\" + packageFullNameWSTR + L".xml\"";
    std::wstring uninstallCommand = L"\"C:\\Program Files (x86)\\Microsoft\\Win7MSIXInstaller\\Win7MSIXinstaller.exe\" -x " + uninstallXmlPath;
    RegWriteKeyValueSZ(HKEY_LOCAL_MACHINE, regPath.c_str(), L"UninstallString", &uninstallCommand, &myXml);
    RegWriteKeyValueSZ(HKEY_LOCAL_MACHINE, regPath.c_str(), L"Publisher", publisher.Get(), &myXml);
    RegWriteKeyValueSZ(HKEY_LOCAL_MACHINE, regPath.c_str(), L"DisplayVersion", const_cast<wchar_t*>(ConvertVersionToString(version).c_str()), &myXml);

    // Clear the window
    InvalidateRgn(hWnd, NULL, TRUE);
    ChangeButtonText(g_closeMessage);
    g_installed = true;
    g_displayCompleteText = true;
    return S_OK;
}

// FUNCTION: FindWindowDisplay
//
// PURPOSE: This compiles the information displayed on the UI when the user selects an msix
//
// windowText: pointer to a wstring that the window message will be saved to
HRESULT DisplayPackageInfo(HWND hWnd, RECT windowRect, std::wstring& displayText, std::wstring& messageText)
{
    int argc = __argc;
    char** argv = __argv;
    auto ParseInput = [&]()->bool
    {
        int index = 1;
        while (index < argc)
        {
            auto option = g_options.find(argv[index]);
            if (option == g_options.end()) { return false; }
            char const *parameter = "";
            if (option->second.TakesParameter)
            {
                if (++index == argc) { break; }
                parameter = argv[index];
            }
            if (!option->second.Callback(parameter)) { return false; }
            ++index;
        }
        return true;
    };

    if (!ParseInput())
    {
        return Help(argv[0]);
    }
    if (g_state.packageName.empty())
    {
        if (g_state.m_uninstallXml.empty())
        {
            return E_FAIL;
        }
        else
        {
            RemovePackage();
            return S_OK;
        }
    }

    g_state.SkipSignature();

    // Create a package using the file name in argv[1]
    ComPtr<IAppxPackageReader> package;
    RETURN_IF_FAILED(GetPackageReader(g_state, &package));

    // Initiates COM pointers to read the manifest
    ComPtr<IAppxManifestReader> manifest;
    RETURN_IF_FAILED(package->GetManifest(&manifest));

    // Get number of payload files for the progress bar
    ComPtr<IAppxFilesEnumerator> fileEnum;
    RETURN_IF_FAILED(package->GetPayloadFiles(&fileEnum));
    BOOL hc = FALSE;
    g_numPayloadFiles = 0;
    RETURN_IF_FAILED(fileEnum->GetHasCurrent(&hc));
    while (hc)
    {
        g_numPayloadFiles++;
        RETURN_IF_FAILED(fileEnum->MoveNext(&hc));
    }
    CreateProgressBar(hWnd, windowRect, g_numPayloadFiles);

    ComPtr<IMsixDocumentElement> domElement;
    RETURN_IF_FAILED(manifest->QueryInterface(UuidOfImpl<IMsixDocumentElement>::iid, reinterpret_cast<void**>(&domElement)));
    ComPtr<IMsixElement> element;
    RETURN_IF_FAILED(domElement->GetDocumentElement(&element));
    ComPtr<IMsixElementEnumerator> veElementEnum;

    // Obtains the Display Name and Logo
    Text<WCHAR> displayName;
    Text<WCHAR> logo;
    std::wstring tmpLogoFile;
    RETURN_IF_FAILED(element->GetElements(
        L"/*[local-name()='Package']/*[local-name()='Applications']/*[local-name()='Application']/*[local-name()='VisualElements']",
        &veElementEnum));

    ComPtr<IStream> logoStream;
    hc = FALSE;
    RETURN_IF_FAILED(veElementEnum->GetHasCurrent(&hc));
    if (hc)
    {
        ComPtr<IMsixElement> visualElementsElement;
        RETURN_IF_FAILED(veElementEnum->GetCurrent(&visualElementsElement));
        RETURN_IF_FAILED(visualElementsElement->GetAttributeValue(L"DisplayName", &displayName));
        RETURN_IF_FAILED(visualElementsElement->GetAttributeValue(L"Square150x150Logo", &logo));
        RETURN_IF_FAILED(GetStreamFromFile(package.Get(), logo.Get(), &logoStream));
    }

    // Get the publisher and version
    ComPtr<IAppxManifestPackageId> manifestId;
    RETURN_IF_FAILED(manifest->GetPackageId(&manifestId));
    Text<WCHAR> publisher;
    RETURN_IF_FAILED(manifestId->GetPublisher(&publisher));
    UINT64 version;
    RETURN_IF_FAILED(manifestId->GetVersion(&version));

    // Show only the CommonName of the publisher
    auto wpublisher = std::wstring(publisher.Get());
    auto cnpub = wpublisher.substr(wpublisher.find_first_of(L"=") + 1,
        wpublisher.find_first_of(L",") - wpublisher.find_first_of(L"=") - 1);

    displayText = L"Install " + std::wstring(displayName.Get()) + L"?";

    messageText = L"Publisher: " + cnpub  + L"\nVersion: " + ConvertVersionToString(version);
    ChangeText(hWnd, displayText, messageText, logoStream.Get());
    return S_OK;
}

void StartParseFile(HWND hWnd)
{
    auto result = ParseAndRun(hWnd);
    if (result != 0)
    {
        std::cout << "Error: " << std::hex << result << " while extracting the appx package" << std::endl;
        Text<char> text;
        auto logResult = GetLogTextUTF8(MyAllocate, &text);
        if (0 == logResult)
        {
            std::cout << "LOG:" << std::endl << text.content << std::endl;
        }
        else
        {
            std::cout << "UNABLE TO GET LOG WITH HR=" << std::hex << logResult << std::endl;
        }
    }
}

void CommandFunc(HWND hWnd, RECT windowRect) {
    std::thread t1(StartParseFile, hWnd);
    t1.detach();
    return;
}

//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    RECT windowRect;
    GetClientRect(hWnd, &windowRect);
    switch (message)
    {
    case WM_CREATE:
        LaunchButton(hWnd, windowRect);
        break;
    case WM_PAINT:
    {
        if (!g_displayInfo)
        {
            HRESULT hr = DisplayPackageInfo(hWnd, windowRect, g_displayText, g_messageText);
            if (FAILED(hr))
            {
                std::wstring failure = L"Loading Package failed";
                g_messageText = L"Failed getting package information with: " + std::to_wstring(hr);
                ChangeText(hWnd, failure, g_messageText);
            }
            g_displayInfo = true;
        }
        if (g_displayCompleteText)
        {
            ChangeText(hWnd, g_state.IsUninstall() ? g_uninstallCompleteTitle : g_completeTitle, g_completeMessage);
            g_displayCompleteText = false;
        }

        break;
    }
    case WM_COMMAND:
        if (!g_installed)
        {
            CommandFunc(hWnd, windowRect);
            return HideButtonWindow();
        }
        else
        {
            PostQuitMessage(0);
            exit(0);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_SIZE:
    case WM_SIZING:
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
}

int CALLBACK WinMain(
    _In_ HINSTANCE hInstance,
    _In_ HINSTANCE hPrevInstance,
    _In_ LPSTR     lpCmdLine,
    _In_ int       nCmdShow
)
{
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = g_windowClass.c_str();
    wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex))
    {
        MessageBox(NULL, L"Call to RegisterClassEx failed!", g_title.c_str(), NULL);
        return 1;
    }
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    CreateInitWindow(hInstance, nCmdShow, g_windowClass, g_title);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
