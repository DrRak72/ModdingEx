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
	SetLiveCoding(false);
	if (!ExecProcessAndLog(UatPath, UatArgs, FText::FromString("UAT BuildCookRun")))
	{
		FileManager.DeleteDirectory(*TempStagingDir, false, true);
		return false;
	}
	SetLiveCoding(true);

	// --- 5. Copy Output from Staging Directory ---
	SlowTask.EnterProgressFrame(1, FText::FromString("Copying build output"));

	FString StagedPaksDir = TempStagingDir / "Windows" / FApp::GetProjectName() / TEXT("Content/Paks");

	if (!FPaths::DirectoryExists(StagedPaksDir))
	{
		FString AltStagedPaksDir = TempStagingDir / FApp::GetProjectName() / TEXT("Content/Paks");
		if (FPaths::DirectoryExists(AltStagedPaksDir)) {
			StagedPaksDir = AltStagedPaksDir;
			UE_LOG(LogModdingEx, Warning, TEXT("Staged Paks directory found at alternate location: %s"), *StagedPaksDir);
		} else {
			UE_LOG(LogModdingEx, Error, TEXT("Staged Paks directory not found after UAT run in expected locations: %s or %s"),
				*(TempStagingDir / PlatformName / FApp::GetProjectName() / TEXT("Content/Paks")), *AltStagedPaksDir);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Build process seemed successful, but the output Paks directory was not found in the staging area. Check UAT logs.")));
		FileManager.DeleteDirectory(*TempStagingDir, false, true);
		return false;
		}
	}

	UE_LOG(LogModdingEx, Log, TEXT("Looking for output files in: %s"), *StagedPaksDir);
	UE_LOG(LogModdingEx, Log, TEXT("Copying output files to: %s"), *FinalDestinationDir);

	// --- Find and Rename/Copy Logic ---
	bool bAllCopiedSuccessfully = true;

	// Handle PAK file(s)
	TArray<FString> FoundPakFiles;
	TArray<FString> FoundUtocFiles;
	TArray<FString> FoundUcasFiles;

	FileManager.FindFiles(FoundPakFiles, *StagedPaksDir, TEXT("*.pak"));
	if (!FoundPakFiles.IsEmpty())
	{
		FString SourcePakPath = StagedPaksDir / FoundPakFiles[0];
		FString DestPakPath = FinalDestinationDir / (ModName + TEXT(".pak")); // Rename using ModName
		UE_LOG(LogModdingEx, Log, TEXT("Copying and renaming PAK: '%s' to '%s'"), *SourcePakPath, *DestPakPath);
		if (FileManager.Copy(*DestPakPath, *SourcePakPath, true, true, true) != COPY_OK)
		{
			UE_LOG(LogModdingEx, Error, TEXT("Failed to copy PAK file: %s -> %s"), *SourcePakPath, *DestPakPath);
			bAllCopiedSuccessfully = false;
		}
		if (FoundPakFiles.Num() > 1) {
			UE_LOG(LogModdingEx, Warning, TEXT("Found %d PAK files in staging directory, but only renamed and copied the first one ('%s'). Additional PAKs ignored:"), FoundPakFiles.Num(), *FoundPakFiles[0]);
			for (int32 i = 1; i < FoundPakFiles.Num(); ++i) {
				UE_LOG(LogModdingEx, Warning, TEXT("  - Ignored: %s"), *FoundPakFiles[i]);
			}
		}
	} else {
		UE_LOG(LogModdingEx, Error, TEXT("No .pak files found in staging directory: %s"), *StagedPaksDir);
		bAllCopiedSuccessfully = false;
	}


	// Handle IO Store files if enabled
	if (bUseIoStore && bAllCopiedSuccessfully)
	{
		FileManager.FindFiles(FoundUtocFiles, *StagedPaksDir, TEXT("*.utoc"));
		if (!FoundUtocFiles.IsEmpty())
		{
			FString SourceUtocPath = StagedPaksDir / FoundUtocFiles[0];
			FString DestUtocPath = FinalDestinationDir / (ModName + TEXT(".utoc")); // Rename using ModName
			UE_LOG(LogModdingEx, Log, TEXT("Copying and renaming UTOC: '%s' to '%s'"), *SourceUtocPath, *DestUtocPath);
			if (FileManager.Copy(*DestUtocPath, *SourceUtocPath, true, true, true) != COPY_OK)
			{
				UE_LOG(LogModdingEx, Error, TEXT("Failed to copy UTOC file: %s -> %s"), *SourceUtocPath, *DestUtocPath);
				bAllCopiedSuccessfully = false;
			}
            if (FoundUtocFiles.Num() > 1) {
                 UE_LOG(LogModdingEx, Warning, TEXT("Found %d UTOC files, only copied/renamed the first ('%s')."), FoundUtocFiles.Num(), *FoundUtocFiles[0]);
            }
		} else {
			UE_LOG(LogModdingEx, Error, TEXT("IO Store enabled, but no .utoc file found in staging directory: %s"), *StagedPaksDir);
			bAllCopiedSuccessfully = false;
		}

		// Handle UCAS file (only if UTOC was okay)
		if (bAllCopiedSuccessfully) {
			FileManager.FindFiles(FoundUcasFiles, *StagedPaksDir, TEXT("*.ucas"));
			if (!FoundUcasFiles.IsEmpty())
			{
				FString SourceUcasPath = StagedPaksDir / FoundUcasFiles[0];
				FString DestUcasPath = FinalDestinationDir / (ModName + TEXT(".ucas")); // Rename using ModName
				UE_LOG(LogModdingEx, Log, TEXT("Copying and renaming UCAS: '%s' to '%s'"), *SourceUcasPath, *DestUcasPath);
				if (FileManager.Copy(*DestUcasPath, *SourceUcasPath, true, true, true) != COPY_OK)
				{
					UE_LOG(LogModdingEx, Error, TEXT("Failed to copy UCAS file: %s -> %s"), *SourceUcasPath, *DestUcasPath);
					bAllCopiedSuccessfully = false;
				}
                if (FoundUcasFiles.Num() > 1) {
                    UE_LOG(LogModdingEx, Warning, TEXT("Found %d UCAS files, only copied/renamed the first ('%s')."), FoundUcasFiles.Num(), *FoundUcasFiles[0]);
                }
			} else {
				UE_LOG(LogModdingEx, Error, TEXT("IO Store enabled, but no .ucas file found in staging directory: %s"), *StagedPaksDir);
				bAllCopiedSuccessfully = false;
			}
		}
	} else if (bUseIoStore && !bAllCopiedSuccessfully) {
        UE_LOG(LogModdingEx, Warning, TEXT("Skipping IOStore file copy due to earlier pak copy failure."));
    }


	// --- 6. Cleanup ---
	UE_LOG(LogModdingEx, Log, TEXT("Cleaning up temporary staging directory: %s"), *TempStagingDir);
	if (!FileManager.DeleteDirectory(*TempStagingDir, false, true)) {
         UE_LOG(LogModdingEx, Warning, TEXT("Could not delete temporary staging directory: %s"), *TempStagingDir);
    }


	// --- 7. Final Notification ---
	if (!bAllCopiedSuccessfully)
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