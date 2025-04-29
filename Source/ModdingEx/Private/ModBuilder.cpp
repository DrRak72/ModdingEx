#include "ModBuilder.h"

#include "FileHelpers.h"
#include "ISettingsModule.h"
#include "ModdingEx.h"
#include "ModdingExSettings.h"
#include "Async/Async.h"
#include "FileUtilities/ZipArchiveWriter.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/ConfigCacheIni.h"
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
#include "Settings/ProjectPackagingSettings.h"
#else
#include "ProjectPackagingSettings.h"
#endif
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Editor.h"
#include "Internationalization/Regex.h"

// Helper function to execute a process and log output
// Returns true on success (ReturnCode 0), false otherwise.
bool ExecProcessAndLog(const FString& Command, const FString& Params, const FText& StepDescription)
{
	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;

	UE_LOG(LogModdingEx, Log, TEXT("Executing Step: %s"), *StepDescription.ToString());
	UE_LOG(LogModdingEx, Log, TEXT("Command: %s %s"), *Command, *Params);

	bool bSuccess = FPlatformProcess::ExecProcess(*Command, *Params, &ReturnCode, &StdOut, &StdErr);

	// Log output regardless of success for debugging
	if (!StdOut.IsEmpty())
	{
		UE_LOG(LogModdingEx, Log, TEXT("StdOut:\n%s"), *StdOut);
	}
	if (!StdErr.IsEmpty())
	{
		// Log standard error as warnings or errors based on return code
		if (ReturnCode != 0) {
			UE_LOG(LogModdingEx, Error, TEXT("StdErr:\n%s"), *StdErr);
		} else {
			UE_LOG(LogModdingEx, Warning, TEXT("StdErr (Warnings):\n%s"), *StdErr);
		}
	}

	if (!bSuccess || ReturnCode != 0)
	{
		UE_LOG(LogModdingEx, Error, TEXT("Execution failed for '%s'. Return Code: %d"), *StepDescription.ToString(), ReturnCode);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Format(TEXT("Step '{0}' failed (Code: {1}). Check logs for details."), {StepDescription.ToString(), ReturnCode})));
		return false;
	}

	UE_LOG(LogModdingEx, Log, TEXT("Execution successful for '%s'."), *StepDescription.ToString());
	return true;
}


// Toggle live coding for building
void SetLiveCoding(bool coding)
{
    GConfig->SetBool(
        TEXT("LiveCoding"),
        TEXT("bEnabled"),
        coding,
        GEditorPerProjectIni
    );
    
    GConfig->Flush(false, GEditorPerProjectIni);
}

bool UModBuilder::BuildMod(const FString& ModName, bool bIsSameContentError)
{
	const UModdingExSettings* Settings = GetDefault<UModdingExSettings>();
	const UProjectPackagingSettings* PackagingSettings = GetDefault<UProjectPackagingSettings>();
	const bool bUseIoStore = PackagingSettings->bUseIoStore;

	FString FinalDestinationDir; 

	SetLiveCoding(false);

	// --- 1. Common Setup ---
	if (Settings->bSaveAllBeforeBuilding)
	{
		FEditorFileUtils::SaveDirtyPackages(false, true, true, false, false, false);
		UE_LOG(LogModdingEx, Log, TEXT("Saved all packages"));
	}

	if (!GetOutputFolder(true, FinalDestinationDir)) 
	{
		if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(
			"Game directory is not set or does not exist in ModdingEx settings. This is required for the output path.\n\nGo to Settings?")) == EAppReturnType::Yes)
	{
			FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "ModdingEx");
		}
		UE_LOG(LogModdingEx, Error, TEXT("Output directory could not be determined from settings."));
		return false;
	}

	// --- 2. Define Paths & Platform ---
	const FString PlatformName = TEXT("Win64");
	FString UatPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Build/BatchFiles/RunUAT.bat"));
	if (!FPaths::FileExists(UatPath))
	{
		UE_LOG(LogModdingEx, Error, TEXT("RunUAT.bat not found at expected location: %s"), *UatPath);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("RunUAT.bat not found. Ensure Engine installation is correct.")));
		return false;
	}
	FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	FString TempStagingDir = FPaths::ProjectIntermediateDir() / TEXT("ModdingExStaging") / FGuid::NewGuid().ToString();

	IFileManager& FileManager = IFileManager::Get();
	if (FPaths::DirectoryExists(TempStagingDir))
	{
		if (!FileManager.DeleteDirectory(*TempStagingDir, false, true)) {
             UE_LOG(LogModdingEx, Warning, TEXT("Could not clean existing temp staging directory: %s"), *TempStagingDir);
        }
	}
	if (!FileManager.MakeDirectory(*TempStagingDir, true))
	{
		UE_LOG(LogModdingEx, Error, TEXT("Failed to create temporary staging directory: %s"), *TempStagingDir);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Format(TEXT("Failed to create temporary staging directory: {0}"), {TempStagingDir})));
		return false;
	}

	UE_LOG(LogModdingEx, Log, TEXT("Using temp staging directory: %s"), *TempStagingDir);
	UE_LOG(LogModdingEx, Log, TEXT("Using final destination directory: %s"), *FinalDestinationDir);

	FScopedSlowTask SlowTask(2, FText::FromString(FString::Format(TEXT("Building {0} via UAT ({1})"), {ModName, bUseIoStore ? TEXT("IO Store + Pak") : TEXT("Pak File")})));
	SlowTask.MakeDialog();

	// --- 3. Construct UAT Arguments ---
	SlowTask.EnterProgressFrame(1, FText::FromString("Running Unreal Automation Tool (BuildCookRun)"));

	FString UatArgs = TEXT("BuildCookRun");
	UatArgs += FString::Printf(TEXT(" -project=\"%s\""), *ProjectPath);
	UatArgs += FString::Printf(TEXT(" -platform=%s"), *PlatformName); 
	UatArgs += TEXT(" -clientconfig=Shipping"); 
	UatArgs += TEXT(" -cook");
	UatArgs += TEXT(" -stage");
	UatArgs += FString::Printf(TEXT(" -stagingdirectory=\"%s\""), *TempStagingDir);
	UatArgs += TEXT(" -package"); 
	UatArgs += TEXT(" -pak");
	UatArgs += TEXT(" -SkipCookingEditorContent");

	if (bUseIoStore) {
		UatArgs += TEXT(" -iostore");
	}

	FString ModContentDir = FPaths::ProjectContentDir() / TEXT("Mods") / ModName;
	if(FPaths::DirectoryExists(ModContentDir))
	{
		UatArgs += FString::Printf(TEXT(" -CookDir=\"%s\""), *ModContentDir);
	}
	else
	{
		UE_LOG(LogModdingEx, Warning, TEXT("Mod content directory not found, cannot specify -CookDir: %s"), *ModContentDir);
	}

	UatArgs += TEXT(" -NoP4");
	UatArgs += TEXT(" -build");
	UatArgs += TEXT(" -utf8output");
	UatArgs += TEXT(" -unattended");
	UatArgs += TEXT(" -nodebuginfo");

	// --- 4. Execute UAT ---
	
	if (!ExecProcessAndLog(UatPath, UatArgs, FText::FromString("UAT BuildCookRun")))
	{
		FileManager.DeleteDirectory(*TempStagingDir, false, true);
		return false;
	}

	// --- 5. Copy Output from Staging Directory ---
	SlowTask.EnterProgressFrame(1, FText::FromString("Copying build output"));

	// StagedPaksDir determination logic remains the same...
	FString StagedPaksDir = TempStagingDir / PlatformName / FApp::GetProjectName() / TEXT("Content/Paks"); // Use PlatformName variable

	// Adjust StagedPaksDir if initial path doesn't exist (same as before)
	if (!FPaths::DirectoryExists(StagedPaksDir))
	{
		// Fallback logic for directory structure variations
		StagedPaksDir = TempStagingDir / FApp::GetProjectName() / TEXT("Content/Paks"); // Try without PlatformName
		if (!FPaths::DirectoryExists(StagedPaksDir)) {
			StagedPaksDir = TempStagingDir / TEXT("Windows") / FApp::GetProjectName() / TEXT("Content/Paks"); // Try specific "Windows" if PlatformName didn't work
		}
		// Add more fallbacks if needed based on observed UAT output structures

		if (!FPaths::DirectoryExists(StagedPaksDir)) {
			UE_LOG(LogModdingEx, Error, TEXT("Staged Paks directory not found after UAT run in expected locations. Check UAT logs. Tried paths ending with: %s"),
				*(PlatformName / FApp::GetProjectName() / TEXT("Content/Paks"))); // Simplified error message
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Build process seemed successful, but the output Paks directory was not found in the staging area. Check UAT logs.")));
			FileManager.DeleteDirectory(*TempStagingDir, false, true); // Cleanup temp dir
			return false;
		} else {
             UE_LOG(LogModdingEx, Warning, TEXT("Staged Paks directory found at alternate location: %s"), *StagedPaksDir);
        }
	}


	UE_LOG(LogModdingEx, Log, TEXT("Looking for output files in: %s"), *StagedPaksDir);
	UE_LOG(LogModdingEx, Log, TEXT("Copying output files to: %s"), *FinalDestinationDir);

	// --- Find Highest Pakchunk Logic ---
	int32 HighestChunkNum = -1;
	FString HighestChunkPakFilename;
	FString BasePlatformString; // String like "-windows" or "-Win64" found in the filename

	TArray<FString> FoundFiles;
	FileManager.FindFiles(FoundFiles, *StagedPaksDir, TEXT("*.pak")); // Find all pak files first

	const FRegexPattern PakChunkPattern(TEXT("^pakchunk(\\d+)(-([^-.]+))?\\.pak$"));


	for (const FString& PakFilename : FoundFiles)
	{
		FRegexMatcher Matcher(PakChunkPattern, PakFilename);
		if (Matcher.FindNext())
		{
			FString ChunkNumStr = Matcher.GetCaptureGroup(1);
			int32 CurrentChunkNum = FCString::Atoi(*ChunkNumStr);

            // Optional: Capture the platform string part for robustness
            FString CurrentPlatformPart = Matcher.GetCaptureGroup(2); // Includes the leading hyphen, e.g., "-Windows"

			UE_LOG(LogModdingEx, Verbose, TEXT("Found pakchunk file: %s, Chunk Number: %d, Platform Part: %s"), *PakFilename, CurrentChunkNum, *CurrentPlatformPart);

			if (CurrentChunkNum > HighestChunkNum)
			{
				HighestChunkNum = CurrentChunkNum;
				HighestChunkPakFilename = PakFilename;
                BasePlatformString = CurrentPlatformPart; // Store the platform string associated with the highest chunk
			}
		}
        else
        {
             UE_LOG(LogModdingEx, Log, TEXT("Found pak file not matching pakchunk pattern, ignoring: %s"), *PakFilename);
             // This might be the base 'ProjectName.pak' or similar, which we might want to ignore anyway
        }
	}

    // --- Copy Files Based on Highest Chunk ---
	bool bEssentialFilesCopied = false;

	if (HighestChunkNum != -1 && !HighestChunkPakFilename.IsEmpty())
	{
		UE_LOG(LogModdingEx, Log, TEXT("Highest pakchunk found: %d (%s). Associated platform string: '%s'"), HighestChunkNum, *HighestChunkPakFilename, *BasePlatformString);

        // Construct expected utoc and ucas names based on the highest pak chunk file found
        FString BaseName = FPaths::GetBaseFilename(HighestChunkPakFilename); // e.g., "pakchunk101-Windows"
		FString HighestChunkUtocFilename = BaseName + TEXT(".utoc");
		FString HighestChunkUcasFilename = BaseName + TEXT(".ucas");

        // Define Source Paths
        FString SourcePakPath = StagedPaksDir / HighestChunkPakFilename;
        FString SourceUtocPath = StagedPaksDir / HighestChunkUtocFilename;
        FString SourceUcasPath = StagedPaksDir / HighestChunkUcasFilename;

        // Define Destination Paths (renamed using ModName)
        FString DestPakPath = FinalDestinationDir / (ModName + TEXT(".pak"));
        FString DestUtocPath = FinalDestinationDir / (ModName + TEXT(".utoc"));
        FString DestUcasPath = FinalDestinationDir / (ModName + TEXT(".ucas"));

        // --- Perform Copying ---
        bool bPakCopied = false;
        bool bUtocCopied = false;
        bool bUcasCopied = false;

        // Copy PAK
		if (FileManager.FileExists(*SourcePakPath)) {
			UE_LOG(LogModdingEx, Log, TEXT("Copying PAK: '%s' to '%s'"), *SourcePakPath, *DestPakPath);
			if (FileManager.Copy(*DestPakPath, *SourcePakPath, true, true, true) == COPY_OK) {
				bPakCopied = true;
			} else {
				UE_LOG(LogModdingEx, Error, TEXT("Failed to copy PAK file: %s -> %s"), *SourcePakPath, *DestPakPath);
			}
        } else {
            UE_LOG(LogModdingEx, Error, TEXT("Highest chunk PAK file '%s' not found in staging directory!"), *HighestChunkPakFilename);
        }

        // Copy UTOC (if IOStore is used)
		if (bUseIoStore) {
			if (FileManager.FileExists(*SourceUtocPath)) {
				UE_LOG(LogModdingEx, Log, TEXT("Copying UTOC: '%s' to '%s'"), *SourceUtocPath, *DestUtocPath);
				if (FileManager.Copy(*DestUtocPath, *SourceUtocPath, true, true, true) == COPY_OK) {
					bUtocCopied = true;
				} else {
					UE_LOG(LogModdingEx, Error, TEXT("Failed to copy UTOC file: %s -> %s"), *SourceUtocPath, *DestUtocPath);
				}
			} else {
				UE_LOG(LogModdingEx, Error, TEXT("IO Store enabled, but expected UTOC file '%s' not found in staging directory!"), *HighestChunkUtocFilename);
			}
        }

        // Copy UCAS (if IOStore is used and UTOC was copied)
		if (bUseIoStore && bUtocCopied) {
            if (FileManager.FileExists(*SourceUcasPath)) {
				UE_LOG(LogModdingEx, Log, TEXT("Copying UCAS: '%s' to '%s'"), *SourceUcasPath, *DestUcasPath);
				if (FileManager.Copy(*DestUcasPath, *SourceUcasPath, true, true, true) == COPY_OK) {
					bUcasCopied = true;
				} else {
					UE_LOG(LogModdingEx, Error, TEXT("Failed to copy UCAS file: %s -> %s"), *SourceUcasPath, *DestUcasPath);
				}
			} else {
				UE_LOG(LogModdingEx, Error, TEXT("IO Store enabled, but expected UCAS file '%s' not found in staging directory!"), *HighestChunkUcasFilename);
			}
        } else if (bUseIoStore && !bUtocCopied) {
            UE_LOG(LogModdingEx, Warning, TEXT("Skipping UCAS file copy because corresponding UTOC was not found or failed to copy."));
        }

        // Determine overall success based on required files
        bEssentialFilesCopied = bPakCopied;
        if (bUseIoStore) {
            bEssentialFilesCopied = bEssentialFilesCopied && bUtocCopied && bUcasCopied;
        }

	} else {
		UE_LOG(LogModdingEx, Error, TEXT("No files matching 'pakchunkN-Platform.pak' pattern found in staging directory: %s"), *StagedPaksDir);
        bEssentialFilesCopied = false; // Mark as failure if no pakchunks found
	}


	// --- 6. Cleanup ---
	UE_LOG(LogModdingEx, Log, TEXT("Cleaning up temporary staging directory: %s"), *TempStagingDir);
	if (!FileManager.DeleteDirectory(*TempStagingDir, false, true)) {
         UE_LOG(LogModdingEx, Warning, TEXT("Could not delete temporary staging directory: %s"), *TempStagingDir);
    }


	// --- 7. Final Notification ---
	if (!bEssentialFilesCopied)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Build completed, but one or more output files were not found or failed to copy to the final destination. Check logs.")));
		return false;
    }

	FNotificationInfo Info(FText::FromString(FString::Format(TEXT("Mod '{0}' built successfully ({1})!"), { ModName, bUseIoStore ? TEXT("IO Store + Pak") : TEXT("Pak File") })));
	Info.Image = FAppStyle::GetBrush(TEXT("LevelEditor.RecompileGameCode"));
	Info.FadeInDuration = 0.1f;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = 3.5f;
	Info.bUseThrobber = false;
	Info.bUseSuccessFailIcons = true;
	Info.bUseLargeFont = true;
	Info.bFireAndForget = false;
	Info.bAllowThrottleWhenFrameRateIsLow = false;
	const auto NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
	NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
	NotificationItem->ExpireAndFadeout();

	if (GEditor) {
	GEditor->PlayEditorSound(TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue"));
	}

	SetLiveCoding(true);

	return true;
}

bool UModBuilder::GetOutputFolder(bool bIsLogicMod, FString& OutFolder)
{
	const auto Settings = GetDefault<UModdingExSettings>();

	// Priority 1: CustomPakDir setting
	if (!Settings->CustomPakDir.Path.IsEmpty())
	{
		FString CustomPath = Settings->CustomPakDir.Path; 
        FPaths::NormalizeDirectoryName(CustomPath);

		if (FPaths::DirectoryExists(CustomPath)) {
			OutFolder = CustomPath;
			UE_LOG(LogModdingEx, Log, TEXT("Using CustomPakDir: %s"), *OutFolder);
			return true;
		} else {
			if (IFileManager::Get().MakeDirectory(*CustomPath, true)) {
				OutFolder = CustomPath;
				UE_LOG(LogModdingEx, Log, TEXT("Created and using CustomPakDir: %s"), *OutFolder);
				return true;
			} else {
				UE_LOG(LogModdingEx, Error, TEXT("CustomPakDir path specified but does not exist and could not be created: %s"), *CustomPath);
				return false;
			}
		}
	}

	// Priority 2: GameDir setting
	if (Settings->GameDir.Path.IsEmpty())
	{
		UE_LOG(LogModdingEx, Error, TEXT("GameDir is not set in ModdingEx settings."));
		return false;
	}

    FString GamePath = Settings->GameDir.Path; 
    FPaths::NormalizeDirectoryName(GamePath); 

	if (!FPaths::DirectoryExists(GamePath)) {
		UE_LOG(LogModdingEx, Error, TEXT("GameDir path does not exist: %s"), *GamePath);
		return false;
	}

	FString RelFolder = bIsLogicMod ? Settings->LogicModFolder : Settings->ContentModFolder;
	if (RelFolder.IsEmpty())
	{
		UE_LOG(LogModdingEx, Error, TEXT("LogicModFolder (or ContentModFolder) is not set in ModdingEx settings."));
		return false;
	}

	RelFolder.ReplaceInline(TEXT("{GameName}"), FApp::GetProjectName());

	OutFolder = FPaths::Combine(GamePath, RelFolder);
    FPaths::NormalizeDirectoryName(OutFolder);
    UE_LOG(LogModdingEx, Log, TEXT("Calculated Output Folder: %s"), *OutFolder);

	if (!FPaths::DirectoryExists(OutFolder))
	{
		if (!IFileManager::Get().MakeDirectory(*OutFolder, true))
		{
			UE_LOG(LogModdingEx, Error, TEXT("Final output folder could not be created: %s"), *OutFolder);
			return false;
		}
		UE_LOG(LogModdingEx, Log, TEXT("Created final output folder: %s"), *OutFolder);
	}

	return true;
}

// Updated ZipModInternal to handle renamed files
bool UModBuilder::ZipModInternal(const FString& ModName)
{
	const auto Settings = GetDefault<UModdingExSettings>();
	const UProjectPackagingSettings* PackagingSettings = GetDefault<UProjectPackagingSettings>();
    bool bLookForIoStoreFiles = false;


	FString OutputDir; 
	if (!GetOutputFolder(true, OutputDir)) 
	{
		return false;
	}

	UE_LOG(LogModdingEx, Log, TEXT("Zipping files from Output directory: %s"), *OutputDir);

	// --- Find files to Zip (using ModName) ---
	IFileManager& FileManager = IFileManager::Get();
	TArray<FString> FilesToArchivePaths;

	FString PakFilePath = OutputDir / (ModName + TEXT(".pak"));
	if (FileManager.FileExists(*PakFilePath)) {
		FilesToArchivePaths.Add(PakFilePath);
	} else {
        UE_LOG(LogModdingEx, Warning, TEXT("Expected pak file '%s' not found in output directory for zipping."), *PakFilePath);
    }

    FString UtocFilePath = OutputDir / (ModName + TEXT(".utoc"));
	FString UcasFilePath = OutputDir / (ModName + TEXT(".ucas"));
    if(FileManager.FileExists(*UtocFilePath) && FileManager.FileExists(*UcasFilePath))
    {
        bLookForIoStoreFiles = true;
        FilesToArchivePaths.Add(UtocFilePath);
		FilesToArchivePaths.Add(UcasFilePath);
        UE_LOG(LogModdingEx, Log, TEXT("Found IOStore files (%s.utoc, %s.ucas) for zipping."), *ModName, *ModName);
    }


	if (FilesToArchivePaths.IsEmpty())
	{
		UE_LOG(LogModdingEx, Error, TEXT("Didn't find any built files named '%s.pak'%s in the output directory '%s' to zip. Make sure you built the mod first."),
            *ModName,
			bLookForIoStoreFiles ? TEXT(" or IOStore files") : TEXT(""),
            *OutputDir);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Didn't find any built files named after the mod to zip. Make sure you built the mod successfully.")));
		return false;
	}

	// --- Prepare Zip File ---
	FString ZipOutputDir = Settings->ModZipDir.Path;
	if (ZipOutputDir.IsEmpty()) {
		ZipOutputDir = FPaths::ProjectSavedDir() / TEXT("Zips");
		UE_LOG(LogModdingEx, Warning, TEXT("ModZipDir not set in settings, using default: %s"), *ZipOutputDir);
	} else {
		ZipOutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ZipOutputDir);
	}
    FPaths::NormalizeDirectoryName(ZipOutputDir);

	if (!FPaths::DirectoryExists(ZipOutputDir) && !FileManager.MakeDirectory(*ZipOutputDir, true))
	{
		UE_LOG(LogModdingEx, Error, TEXT("Zips output directory does not exist and could not be created: %s"), *ZipOutputDir);
		if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(
									 "Zip output directory does not exist/could not be created. Go to settings?")) == EAppReturnType::Yes)
		{
			FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Project", "Plugins", "ModdingEx");
		}
		return false;
	}

	const FString ZipFilePath = ZipOutputDir / (ModName + ".zip");
	UE_LOG(LogModdingEx, Log, TEXT("Creating zip file at: %s"), *ZipFilePath);

	// --- Create Zip Archive ---
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> ZipFileHandle(PlatformFile.OpenWrite(*ZipFilePath));

	if (!ZipFileHandle)
	{
		UE_LOG(LogModdingEx, Error, TEXT("Failed to open zip file for writing: %s"), *ZipFilePath);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Failed to open zip file for writing.")));
		return false;
	}

    TUniquePtr<FZipArchiveWriter> ZipWriter = MakeUnique<FZipArchiveWriter>(ZipFileHandle.Get());

	bool bAllFilesAdded = true;
	for (const FString& FullPathToFile : FilesToArchivePaths)
	{
		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *FullPathToFile))
		{
			UE_LOG(LogModdingEx, Error, TEXT("Failed to read file data for zipping: %s"), *FullPathToFile);
			bAllFilesAdded = false;
			continue;
		}

		FString FileNameInZip = FPaths::GetCleanFilename(FullPathToFile);
		UE_LOG(LogModdingEx, Log, TEXT("Adding '%s' to zip archive."), *FileNameInZip);
		ZipWriter->AddFile(FileNameInZip, FileData, FDateTime::Now());
	}

    ZipWriter.Reset();
    ZipFileHandle.Reset();


	if (!bAllFilesAdded) {
        UE_LOG(LogModdingEx, Error, TEXT("One or more files could not be read and added to the zip archive: %s"), *ZipFilePath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Failed to read some files while creating the zip. Check logs.")));
        return false;
    }

	// --- Success Notification ---
	FNotificationInfo Info(FText::FromString(FString::Format(TEXT("Mod '{0}' zipped successfully!"), {ModName})));
	Info.Image = FAppStyle::GetBrush(TEXT("LevelEditor.RecompileGameCode"));
	Info.FadeInDuration = 0.1f;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = 3.5f;
	Info.bUseThrobber = false;
	Info.bUseSuccessFailIcons = true;
	Info.bUseLargeFont = true;
	Info.bFireAndForget = false;
	Info.bAllowThrottleWhenFrameRateIsLow = false;
	const auto NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
	NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
	NotificationItem->ExpireAndFadeout();

	if (GEditor) {
	GEditor->PlayEditorSound(TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue"));
	}

	if(Settings->bOpenZipFolderAfterZipping) {
		FPlatformProcess::ExploreFolder(*ZipOutputDir);
    }

	return true;
}

bool UModBuilder::ZipMod(const FString& ModName)
{
	const auto Settings = GetDefault<UModdingExSettings>();

	if (Settings->bAlwaysBuildBeforeZipping)
	{
		UE_LOG(LogModdingEx, Log, TEXT("Building mod '%s' before zipping (using UAT)..."), *ModName);
		if (!BuildMod(ModName))
		{
			UE_LOG(LogModdingEx, Error, TEXT("Failed to zip mod '%s' because the UAT build failed."), *ModName);
		return false;
	}
        UE_LOG(LogModdingEx, Log, TEXT("Build successful, proceeding to zip mod '%s'..."), *ModName);
}
	return ZipModInternal(ModName);
}