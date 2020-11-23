


#include "ShaderCompiler.h"
#include "GenericPlatform/GenericPlatformFile.h"

#include "Core/Public/Misc/ConfigCacheIni.h"
#include "Core/Public/CoreGlobals.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"


#if PLATFORM_WINDOWS // FASTBuild shader compilation is only supported on Windows.

//you can modify it by config
static FString FASTBuild_CachePath(TEXT("\\\\192.168.1.1\\samba\\FastBuildCache\\ShaderCacheWith4.24"));
static const FString FASTBuild_OrbisSDKToolchainRoot(TEXT("Engine\\Binaries\\ThirdParty\\PS4\\OrbisSDK"));

namespace FASTBuildConsoleVariables {
	/** The total number of batches to fill with shaders before creating another group of batches. */
	bool bForceRemoteCompile = false;
	bool bDisableCache = false;
	bool bDisableRemoteCompile = false;
	int32 BatchGroupSize = 128;
	FAutoConsoleVariableRef CVarFASTBuildShaderCompileBatchGroupSize(
		TEXT("r.FASTBuildShaderCompile.BatchGroupSize"),
		BatchGroupSize,
		TEXT("Specifies the number of batches to fill with shaders.\n")
		TEXT("Shaders are spread across this number of batches until all the batches are full.\n")
		TEXT("This allows the FASTBuild compile to go wider when compiling a small number of shaders.\n")
		TEXT("Default = 128\n"),
		ECVF_Default);

	bool Enabled = true;
	FAutoConsoleVariableRef CVarFASTBuildShaderCompile(
		TEXT("r.FASTBuildShaderCompile"),
		Enabled,
		TEXT("Enables or disables the use of FASTBuild to build shaders.\n")
		TEXT("0: Local builds only. \n")
		TEXT("1: Distribute builds using FASTBuild."),
		ECVF_Default);

	FAutoConsoleVariableRef CVarFASTBuildShaderCompileDisableRemote(
		TEXT("r.FASTBuildShaderCompile.DisableRemote"),
		bDisableRemoteCompile,
		TEXT("disables the use of FASTBuild to build shaders (false/true).\n"),
		ECVF_Default);

	/** The maximum number of shaders to group into a single FASTBuild task. */
	int32 BatchSize = 128;
	FAutoConsoleVariableRef CVarFASTBuildShaderCompileBatchSize(
		TEXT("r.FASTBuildShaderCompile.BatchSize"),
		BatchSize,
		TEXT("Specifies the number of shaders to batch together into a single FASTBuild task.\n")
		TEXT("Default = 16\n"),
		ECVF_Default);

	/**
	* The number of seconds to wait after a job is submitted before kicking off the XGE process.
	* This allows time for the engine to enqueue more shaders, so we get better batching.
	*/
	float JobTimeout = 0.5f;
	FAutoConsoleVariableRef CVarXGEShaderCompileJobTimeout(
		TEXT("r.FASTBuildShaderCompile.JobTimeout"),
		JobTimeout,
		TEXT("The number of seconds to wait for additional shader jobs to be submitted before starting a build.\n")
		TEXT("Default = 0.5\n"),
		ECVF_Default);

	
	void Init() {
		static bool bInitialized = false;
		if (!bInitialized) {
			
			GConfig->GetBool(TEXT("DevOptions.Shaders"),
				TEXT("bAllowUseFastBuild"),
							 FASTBuildConsoleVariables::Enabled,
							 GEngineIni);

			GConfig->GetBool(TEXT("DevOptions.Shaders"),
							 TEXT("bFastBuildOptForceRemote"),
							 FASTBuildConsoleVariables::bForceRemoteCompile,
							 GEngineIni);

			GConfig->GetBool(TEXT("DevOptions.Shaders"),
							 TEXT("bFastBuildOptDisableCache"),
							 FASTBuildConsoleVariables::bDisableCache,
							 GEngineIni);

			FString CachePath = GConfig->GetStr(TEXT("DevOptions.Shaders"),
							 TEXT("FastBuildOptCachePath"),
							 GEngineIni);

			if(!CachePath.IsEmpty()){
				FASTBuild_CachePath = CachePath;
			}	

			
			// Allow command line to override the value of the console variables.
			if (FParse::Param(FCommandLine::Get(), TEXT("xgeshadercompile"))) {
				FASTBuildConsoleVariables::Enabled = true;
			}
			if (FParse::Param(FCommandLine::Get(), TEXT("noxgeshadercompile")) || FParse::Param(FCommandLine::Get(), TEXT("noshaderworker"))) {
				FASTBuildConsoleVariables::Enabled = false;
			}

			bInitialized = true;
		}
	}

}

static FString GetFASTBuild_ExecutablePath() {

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FString path1st = FPaths::ConvertRelativePathToFull(FPaths::EngineDir()) / TEXT("Extras/FASTBuild/Win64/FBuild.exe");
	//auto AbsFBuildExecutablePath = FPaths::EngineDir() / GetFASTBuild_ExecutablePath();
	if (!PlatformFile.FileExists(*path1st)) {
		//todo search from path
		
	}
	return path1st;
}


//static const FString FASTBuild_ToolchainExtraFile[]{TEXT("Engine\\Binaries\\ThirdParty\\Windows\\DirectX\\x64\\d3dcompiler_47.dll"),};

static const FString FASTBuild_ToolchainDir[]
{
	TEXT("Engine\\Binaries\\ThirdParty\\Windows\\DirectX\\x64"),
	TEXT("Engine\\Binaries\\ThirdParty\\AppLocalDependencies\\Win64"),
};


static const FString FASTBuild_ScriptFileName(TEXT("fbshader.bff"));
static const FString FASTBuild_InputFileName(TEXT("Worker.fbin"));
static const FString FASTBuild_OutputFileName(TEXT("Worker.fbout"));
static const FString FASTBuild_SuccessFileName(TEXT("Success"));

bool FShaderCompileFASTBuildThreadRunnable::IsEnabled(){
	return FASTBuildConsoleVariables::Enabled;
}


/** Initialization constructor. */
FShaderCompileFASTBuildThreadRunnable::FShaderCompileFASTBuildThreadRunnable(class FShaderCompilingManager* InManager)
	: FShaderCompileThreadRunnableBase(InManager)
	, BuildProcessID(INDEX_NONE)
	, ShaderBatchesInFlightCompleted(0)
	, FASTBuildWorkingDirectory(InManager->AbsoluteShaderBaseWorkingDirectory / TEXT("FASTBuild"))
	, FASTBuildDirectoryIndex(0)
	, LastAddTime(0)
	, StartTime(0)
	, BatchIndexToCreate(0)
	, BatchIndexToFill(0) {}

FShaderCompileFASTBuildThreadRunnable::~FShaderCompileFASTBuildThreadRunnable() {
	if (BuildProcessHandle.IsValid()) {
		// We still have a build in progress.
		// Kill it...
		FPlatformProcess::TerminateProc(BuildProcessHandle);
		FPlatformProcess::CloseProc(BuildProcessHandle);
	}

	// Clean up any intermediate files/directories we've got left over.
	IFileManager::Get().DeleteDirectory(*FASTBuildWorkingDirectory, false, true);

	// Delete all the shader batch instances we have.
	for (FShaderBatch* Batch : ShaderBatchesIncomplete)
		delete Batch;

	for (FShaderBatch* Batch : ShaderBatchesInFlight)
		delete Batch;

	for (FShaderBatch* Batch : ShaderBatchesFull)
		delete Batch;

	ShaderBatchesIncomplete.Empty();
	ShaderBatchesInFlight.Empty();
	ShaderBatchesFull.Empty();
}

static FArchive* _CreateFileHelper(const FString& Filename)
{
	// TODO: This logic came from FShaderCompileThreadRunnable::WriteNewTasks().
	// We can't avoid code duplication unless we refactored the local worker too.

	FArchive* File = nullptr;
	int32 RetryCount = 0;
	// Retry over the next two seconds if we can't write out the file.
	// Anti-virus and indexing applications can interfere and cause this to fail.
	while (File == nullptr && RetryCount < 200)
	{
		if (RetryCount > 0)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		File = IFileManager::Get().CreateFileWriter(*Filename, FILEWRITE_EvenIfReadOnly);
		RetryCount++;
	}
	if (File == nullptr)
	{
		File = IFileManager::Get().CreateFileWriter(*Filename, FILEWRITE_EvenIfReadOnly | FILEWRITE_NoFail);
	}
	checkf(File, TEXT("Failed to create file %s!"), *Filename);
	return File;
}

static void _MoveFileHelper(const FString& To, const FString& From)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (PlatformFile.FileExists(*From))
	{
		FString DirectoryName;
		int32 LastSlashIndex;
		if (To.FindLastChar('/', LastSlashIndex))
		{
			DirectoryName = To.Left(LastSlashIndex);
		}
		else
		{
			DirectoryName = To;
		}

		// TODO: This logic came from FShaderCompileThreadRunnable::WriteNewTasks().
		// We can't avoid code duplication unless we refactored the local worker too.

		bool Success = false;
		int32 RetryCount = 0;
		// Retry over the next two seconds if we can't move the file.
		// Anti-virus and indexing applications can interfere and cause this to fail.
		while (!Success && RetryCount < 200)
		{
			if (RetryCount > 0)
			{
				FPlatformProcess::Sleep(0.01f);
			}

			// MoveFile does not create the directory tree, so try to do that now...
			Success = PlatformFile.CreateDirectoryTree(*DirectoryName);
			if (Success)
			{
				Success = PlatformFile.MoveFile(*To, *From);
			}
			RetryCount++;
		}
		checkf(Success, TEXT("Failed to move file %s to %s!"), *From, *To);
	}
}

static void _DeleteFileHelper(const FString& Filename)
{
	// TODO: This logic came from FShaderCompileThreadRunnable::WriteNewTasks().
	// We can't avoid code duplication unless we refactored the local worker too.

	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename))
	{
		bool bDeletedOutput = IFileManager::Get().Delete(*Filename, true, true);

		// Retry over the next two seconds if we couldn't delete it
		int32 RetryCount = 0;
		while (!bDeletedOutput && RetryCount < 200)
		{
			FPlatformProcess::Sleep(0.01f);
			bDeletedOutput = IFileManager::Get().Delete(*Filename, true, true);
			RetryCount++;
		}
		checkf(bDeletedOutput, TEXT("Failed to delete %s!"), *Filename);
	}
}



static bool _GetCommonBaseDir(const FString & dir1, const FString &dir2, FString& basedir) {
	FString d1 = FPaths::ConvertRelativePathToFull(dir1);
	FString d2 = FPaths::ConvertRelativePathToFull(dir2);
	basedir.Empty();

	d1.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	d2.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	TArray<FString>  d1Array;
	d1.ParseIntoArray(d1Array, TEXT("/"), true);

	TArray<FString>  d2Array;
	d2.ParseIntoArray(d2Array, TEXT("/"), true);

	int idx = 0;
	while (idx < d1Array.Num() && idx < d2Array.Num()) {
		if (d1Array[idx].Compare(d2Array[idx], ESearchCase::IgnoreCase) == 0) {
			++idx;
		}
		else {
			break;
		}
	}

	if (idx == 0) {
		return false;
	}


	for (int i = 0; i < idx; ++i) {
		basedir += d1Array[i];
		if (i + 1 < idx) {
			basedir += TEXT("/");
		}
	}
	return true;
}


bool FShaderCompileFASTBuildThreadRunnable::IsSupported() {

	FASTBuildConsoleVariables::Init();

	// Check to see if the FASTBuild exe exists.
	if (FASTBuildConsoleVariables::Enabled) {
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		//auto AbsFBuildExecutablePath = FPaths::EngineDir() / GetFASTBuild_ExecutablePath();
		if (!PlatformFile.FileExists(*GetFASTBuild_ExecutablePath())) {
			UE_LOG(LogShaderCompilers, Warning, TEXT("Cannot use FASTBuild Shader Compiler as FASTBuild is not found. Target Path:%s"), *GetFASTBuild_ExecutablePath());
			FASTBuildConsoleVariables::Enabled = false;
		}
		else {
			UE_LOG(LogShaderCompilers, Warning, TEXT("FASTBuild Shader Compiler found. Target Path:%s"), *GetFASTBuild_ExecutablePath());
		}

		//need check same base for shader
		FString basedir = FPaths::RootDir();
		const auto DirMapping = AllShaderSourceDirectoryMappings();
		for (const auto & me : DirMapping) {
			if (!_GetCommonBaseDir(me.Value, basedir, basedir)) {
				UE_LOG(LogShaderCompilers, Warning, TEXT("FASTBuild Shader Compiler Not Enabled for shader mapping not in same dir(%s,%s)"),
					   *me.Value, *basedir);
				FASTBuildConsoleVariables::Enabled = false;
			}
		}

	}


	return FASTBuildConsoleVariables::Enabled;
}


void FShaderCompileFASTBuildThreadRunnable::PostCompletedJobsForBatch(FShaderBatch* Batch) {
	// Enter the critical section so we can access the input and output queues
	FScopeLock Lock(&Manager->CompileQueueSection);
	for (FShaderCommonCompileJob* Job : Batch->GetJobs()) {
		FShaderMapCompileResults& ShaderMapResults = Manager->ShaderMapJobs.FindChecked(Job->Id);
		ShaderMapResults.FinishedJobs.Add(Job);
		ShaderMapResults.bAllJobsSucceeded = ShaderMapResults.bAllJobsSucceeded && Job->bSucceeded;
	}

	// Using atomics to update NumOutstandingJobs since it is read outside of the critical section
	FPlatformAtomics::InterlockedAdd(&Manager->NumOutstandingJobs, -Batch->NumJobs());
}

void FShaderCompileFASTBuildThreadRunnable::FShaderBatch::AddJob(FShaderCommonCompileJob* Job) {
	// We can only add jobs to a batch which hasn't been written out yet.
	if (bTransferFileWritten) {
		UE_LOG(LogShaderCompilers, Fatal, TEXT("Attempt to add shader compile jobs to a FASTBuild shader batch which has already been written to disk."));
	}
	else {
		Jobs.Add(Job);
	}
}

void FShaderCompileFASTBuildThreadRunnable::FShaderBatch::WriteTransferFile() {
	// Write out the file that the worker app is waiting for, which has all the information needed to compile the shader.
	FArchive* TransferFile = _CreateFileHelper(InputFileNameAndPath);
	FShaderCompileUtilities::DoWriteTasks(Jobs, *TransferFile);
	delete TransferFile;

	bTransferFileWritten = true;
}

void FShaderCompileFASTBuildThreadRunnable::FShaderBatch::SetIndices(int32 InDirectoryIndex, int32 InBatchIndex) {
	DirectoryIndex = InDirectoryIndex;
	BatchIndex = InBatchIndex;

	WorkingDirectory = FString::Printf(TEXT("%s/%d/%d"), *DirectoryBase, DirectoryIndex, BatchIndex);

	InputFileNameAndPath = WorkingDirectory / InputFileName;
	OutputFileNameAndPath = WorkingDirectory / OutputFileName;
	SuccessFileNameAndPath = WorkingDirectory / SuccessFileName;
}

void FShaderCompileFASTBuildThreadRunnable::FShaderBatch::CleanUpFiles(bool keepInputFile) {
	if (!keepInputFile)
	{
		_DeleteFileHelper(InputFileNameAndPath);
	}

	_DeleteFileHelper(OutputFileNameAndPath);
	_DeleteFileHelper(SuccessFileNameAndPath);
}

static void WriteFASTBuildScriptFileHeader(FArchive* ScriptFile, const FString& WorkerName) {
	static const TCHAR HeaderTemplate[] =
		TEXT("Settings\r\n")
		TEXT("{\r\n")
		TEXT("\t.CachePath = '%s'\r\n")
		TEXT("}\r\n")
		TEXT("\r\n")
		TEXT("Compiler('ShaderCompiler')\r\n")
		TEXT("{\r\n")
		TEXT("\t.CompilerFamily = 'custom'\r\n")
		TEXT("\t.Executable = '%s'\r\n")
		TEXT("\t.ExecutableRootPath = '%s'\r\n")
		TEXT("\t.SimpleDistributionMode = true\r\n")
		TEXT("\t.CustomEnvironmentVariables = { 'SCE_ORBIS_SDK_DIR=%%1%s' }\r\n")
		TEXT("\t.ExtraFiles = \r\n")
		TEXT("\t{\r\n");

	FString BaseRootDir = FPaths::RootDir();

	FString HeaderString = FString::Printf(HeaderTemplate, 
			*FASTBuild_CachePath, *WorkerName, 
			*IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*BaseRootDir), 
			*FASTBuild_OrbisSDKToolchainRoot);

	ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*HeaderString, HeaderString.Len()).Get(), 
											sizeof(ANSICHAR) * HeaderString.Len());
	class FDependencyEnumerator : public IPlatformFile::FDirectoryVisitor {
	public:
		FDependencyEnumerator(FArchive* InScriptFile, const TCHAR* InPrefix, const TCHAR* InExtension,const TCHAR* InExcludeExtensions=NULL)
			: ScriptFile(InScriptFile)
			, Prefix(InPrefix)
			, Extension(InExtension) 
		{		
			if(InExcludeExtensions != NULL){
			
				FString Extersion = InExcludeExtensions;
				while(true){				
					FString  CurExt = TEXT("");
					FString LeftExts = TEXT("");
					if(Extersion.Split("|", &CurExt, &LeftExts)){
						ExcludeExtersions.Add(CurExt);
						Extersion = LeftExts;
					}
					else {
						ExcludeExtersions.Add(Extersion);
						break;
					}
				}
			}			
		}

		virtual bool Visit(const TCHAR* FilenameChar, bool bIsDirectory) override {
			if (!bIsDirectory) {
				FString Filename = FString(FilenameChar);

				FString FileExtension = FPaths::GetExtension(Filename);
				bool Excluded = ExcludeExtersions.Find(FileExtension) != INDEX_NONE;

				if (!Excluded && (!Prefix || Filename.Contains(Prefix)) && (!Extension || Filename.EndsWith(Extension))) {
					FString ExtraFile = TEXT("\t\t'") + IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*Filename) + TEXT("',\r\n");
					ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*ExtraFile, ExtraFile.Len()).Get(), sizeof(ANSICHAR) * ExtraFile.Len());
				}
			}

			return true;
		}

		FArchive* const ScriptFile;
		const TCHAR* Prefix;
		const TCHAR* Extension;
		TArray<FString> ExcludeExtersions;
	};


	//deps
	//tool chain file
	//for (const FString& ExtraFilePartialPath : FASTBuild_ToolchainExtraFile) {
	//	FString ExtraFile = TEXT("\t\t'") + IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*(FPaths::RootDir() / ExtraFilePartialPath)) + TEXT("',\r\n");
	//	ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*ExtraFile, ExtraFile.Len()).Get(), sizeof(ANSICHAR) * ExtraFile.Len());
	//}

	//tool chain path
	FDependencyEnumerator AllFileVisitor = FDependencyEnumerator(ScriptFile, NULL, NULL);
	for (const FString& toolPath : FASTBuild_ToolchainDir) {
		IFileManager::Get().IterateDirectoryRecursively(*FPaths::Combine(BaseRootDir, toolPath), AllFileVisitor);
	}

	FDependencyEnumerator ModuleDeps = FDependencyEnumerator(ScriptFile, TEXT("ShaderCompileWorker"), nullptr, TEXT("pdb|exe"));
	IFileManager::Get().IterateDirectoryRecursively(*FPlatformProcess::GetModulesDirectory(), ModuleDeps);

	const auto DirMapping = AllShaderSourceDirectoryMappings();
	for(const auto & me: DirMapping){		
		IFileManager::Get().IterateDirectoryRecursively(*me.Value, AllFileVisitor);
	}

	const FString ExtraFilesFooter =
		TEXT("\t}\r\n")
		TEXT("}\r\n");
	ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*ExtraFilesFooter, ExtraFilesFooter.Len()).Get(), sizeof(ANSICHAR) * ExtraFilesFooter.Len());

}

static void WriteFASTBuildScriptFileFooter(FArchive* ScriptFile) {}

void FShaderCompileFASTBuildThreadRunnable::GatherResultsFromFASTBuild() {
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	IFileManager& FileManager = IFileManager::Get();

	// Reverse iterate so we can remove batches that have completed as we go.
	for (int32 Index = ShaderBatchesInFlight.Num() - 1; Index >= 0; Index--) {
		FShaderBatch* Batch = ShaderBatchesInFlight[Index];

		// If this batch is completed already, skip checks.
		if (Batch->bSuccessfullyCompleted) {
			continue;
		}

		// Instead of checking for another file, we just check to see if the file exists, and if it does, we check if it has a write lock on it. FASTBuild never does partial writes, so this should tell us if FASTBuild is complete.
		// Perform the same checks on the worker output file to verify it came from this build.
		if (PlatformFile.FileExists(*Batch->OutputFileNameAndPath)) {
			if (FileManager.FileSize(*Batch->OutputFileNameAndPath) > 0) {
				IFileHandle* Handle = PlatformFile.OpenWrite(*Batch->OutputFileNameAndPath, true);
				if (Handle) {
					delete Handle;

					if (PlatformFile.GetTimeStamp(*Batch->OutputFileNameAndPath) >= ScriptFileCreationTime) {
						FArchive* OutputFilePtr = FileManager.CreateFileReader(*Batch->OutputFileNameAndPath, FILEREAD_Silent);
						if (OutputFilePtr) {
							FArchive& OutputFile = *OutputFilePtr;
							FShaderCompileUtilities::DoReadTaskResults(Batch->GetJobs(), OutputFile);

							// Close the output file.
							delete OutputFilePtr;

							// Cleanup the worker files
							// Do NOT clean up files until the whole batch is done, so we can clean them all up once the fastbuild process exits. Otherwise there is a race condition between FastBuild checking the output files, and us deleting them here.
							//Batch->CleanUpFiles(false);			// (false = don't keep the input file)
							Batch->bSuccessfullyCompleted = true;
							PostCompletedJobsForBatch(Batch);
							//ShaderBatchesInFlight.RemoveAt(Index);
							ShaderBatchesInFlightCompleted++;
							//delete Batch;
						}
					}
				}
			}
		}
	}
}

int32 FShaderCompileFASTBuildThreadRunnable::CompilingLoop() {
	bool bWorkRemaining = false;

	// We can only run one XGE build at a time.
	// Check if a build is currently in progress.
	if (BuildProcessHandle.IsValid()) {
		// Read back results from the current batches in progress.
		GatherResultsFromFASTBuild();

		bool bDoExitCheck = false;
		if (FPlatformProcess::IsProcRunning(BuildProcessHandle)) {
			if (ShaderBatchesInFlight.Num() == ShaderBatchesInFlightCompleted) {
				// We've processed all batches.
				// Wait for the XGE console process to exit
				FPlatformProcess::WaitForProc(BuildProcessHandle);
				bDoExitCheck = true;
			}
		}
		else {
			bDoExitCheck = true;
		}

		if (bDoExitCheck) {
			int iShaderBatchNumInFlight =  ShaderBatchesInFlight.Num();
			if (iShaderBatchNumInFlight > ShaderBatchesInFlightCompleted) {
				// The build process has stopped.
				// Do one final pass over the output files to gather any remaining results.
				GatherResultsFromFASTBuild();
			}

			// The build process is no longer running.
			// We need to check the return code for possible failure
			int32 ReturnCode = 0;
			FPlatformProcess::GetProcReturnCode(BuildProcessHandle, &ReturnCode);
			/*
			    FBUILD_BUILD_FAILED                     = -1,
				FBUILD_ERROR_LOADING_BFF                = -2,
				FBUILD_BAD_ARGS                         = -3,
				FBUILD_ALREADY_RUNNING                  = -4,
				FBUILD_FAILED_TO_SPAWN_WRAPPER          = -5,
				FBUILD_FAILED_TO_SPAWN_WRAPPER_FINAL    = -6,
				FBUILD_WRAPPER_CRASHED                  = -7,			
			*/
			switch (ReturnCode) {
				case 0:
				// No error
				break;

				case 1:
				// One or more of the shader compile worker processes crashed.
				UE_LOG(LogShaderCompilers, Fatal, TEXT("An error occurred during an XGE shader compilation job. One or more of the shader compile worker processes exited unexpectedly (Code 1)."));
				break;

				case 2:
				// Fatal IncrediBuild error
				UE_LOG(LogShaderCompilers, Fatal, TEXT("An error occurred during an FASTBuild shader compilation job. XGConsole.exe returned a fatal Incredibuild error (Code 2)."));
				break;

				case 3:
				// User canceled the build
				UE_LOG(LogShaderCompilers, Display, TEXT("The user terminated an XGE shader compilation job. Incomplete shader jobs will be redispatched in another FASTBuild build."));
				break;

				default:
				UE_LOG(LogShaderCompilers, Display, TEXT("An unknown error occurred during an XGE shader compilation job (Code %d). shader batch num (%d,%d) Incomplete shader jobs will be redispatched in another FASTBuild build."),
					ReturnCode, iShaderBatchNumInFlight, ShaderBatchesInFlightCompleted );
				//use local build
				FASTBuildConsoleVariables::bDisableRemoteCompile = true;
				break;
			}


			// Reclaim jobs from the workers which did not succeed (if any).
			for (int i = 0; i < ShaderBatchesInFlight.Num(); ++i) {
				FShaderBatch* Batch = ShaderBatchesInFlight[i];

				if (Batch->bSuccessfullyCompleted) {
					// If we completed successfully, clean up.
					//PostCompletedJobsForBatch(Batch);
					Batch->CleanUpFiles(false);

					// This will be a dangling pointer until we clear the array at the end of this for loop
					delete Batch;
				}
				else {

					// Delete any output/success files, but keep the input file so we don't have to write it out again.
					Batch->CleanUpFiles(true);

					// We can't add any jobs to a shader batch which has already been written out to disk,
					// so put the batch back into the full batches list, even if the batch isn't full.
					ShaderBatchesFull.Add(Batch);

					// Reset the batch/directory indices and move the input file to the correct place.
					FString OldInputFilename = Batch->InputFileNameAndPath;
					Batch->SetIndices(FASTBuildDirectoryIndex, BatchIndexToCreate++);
					_MoveFileHelper(Batch->InputFileNameAndPath, OldInputFilename);
				}
			}
			ShaderBatchesInFlightCompleted = 0;
			ShaderBatchesInFlight.Empty();
			FPlatformProcess::CloseProc(BuildProcessHandle);
		}

		bWorkRemaining |= ShaderBatchesInFlight.Num() > ShaderBatchesInFlightCompleted;
	}
	// No build process running. Check if we can kick one off now.
	else {
		// Determine if enough time has passed to allow a build to kick off.
		// Since shader jobs are added to the shader compile manager asynchronously by the engine, 
		// we want to give the engine enough time to queue up a large number of shaders.
		// Otherwise we will only be kicking off a small number of shader jobs at once.
		bool BuildDelayElapsed = (((FPlatformTime::Cycles() - LastAddTime) * FPlatformTime::GetSecondsPerCycle()) >= FASTBuildConsoleVariables::JobTimeout);
		bool HasJobsToRun = (ShaderBatchesIncomplete.Num() > 0 || ShaderBatchesFull.Num() > 0);

		if (BuildDelayElapsed && HasJobsToRun && ShaderBatchesInFlight.Num() == ShaderBatchesInFlightCompleted) {
			// Move all the pending shader batches into the in-flight list.
			ShaderBatchesInFlight.Reserve(ShaderBatchesIncomplete.Num() + ShaderBatchesFull.Num());

			for (FShaderBatch* Batch : ShaderBatchesIncomplete) {
				// Check we've actually got jobs for this batch.
				check(Batch->NumJobs() > 0);

				// Make sure we've written out the worker files for any incomplete batches.
				Batch->WriteTransferFile();
				ShaderBatchesInFlight.Add(Batch);
			}

			for (FShaderBatch* Batch : ShaderBatchesFull) {
				// Check we've actually got jobs for this batch.
				check(Batch->NumJobs() > 0);

				ShaderBatchesInFlight.Add(Batch);
			}

			ShaderBatchesFull.Empty();
			ShaderBatchesIncomplete.Empty(FASTBuildConsoleVariables::BatchGroupSize);

			FString ScriptFilename = FASTBuildWorkingDirectory / FString::FromInt(FASTBuildDirectoryIndex) / FASTBuild_ScriptFileName;

			//
			static int FbBuildSeq = 0;
			UE_LOG(LogShaderCompilers, Log, TEXT("Generate FastBuildShaderScript Path:%s ProjectDir:%s FilePath:%s IsProjectFilePathSet:%d CacheEnable:%d CachePath:%s"),
				*ScriptFilename, *FPaths::ProjectDir(), *FPaths::GetProjectFilePath(), FPaths::IsProjectFilePathSet(),
				   FASTBuildConsoleVariables::bDisableCache == false, *FASTBuild_CachePath);
			for(;;++FbBuildSeq){
				auto TryFileName = ScriptFilename.Replace(TEXT(".bff"), *FString::Printf(TEXT(".%04d.bff"), FbBuildSeq));
				if (!FPaths::FileExists(TryFileName)){
					ScriptFilename = TryFileName;
					break;
				}
			}

			// Create the FASTBuild script file.
			FArchive* ScriptFile = _CreateFileHelper(ScriptFilename);
			WriteFASTBuildScriptFileHeader(ScriptFile, Manager->ShaderCompileWorkerName);

			// Write the XML task line for each shader batch
			for (FShaderBatch* Batch : ShaderBatchesInFlight) {
				FString WorkerAbsoluteDirectory = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*Batch->WorkingDirectory);
				FPaths::NormalizeDirectoryName(WorkerAbsoluteDirectory);

				// Proper path should be "WorkingDir" 0 0 "Worker.in" "Worker.out"
				//16028 0 "C:\\tmp\\0\\Worker.in" "C:\\tmp\\0\\Worker.out" -xge_xml -Multiprocess
				FString ExecFunction = FString::Printf(
					TEXT("ObjectList('ShaderBatch-%d')\r\n")
					TEXT("{\r\n")
					TEXT("\t.Compiler = 'ShaderCompiler'\r\n")
					TEXT("\t.CompilerOptions = '\"\" %d %d \"%%1\" \"%%2\" -xge_xml %s '\r\n")
					TEXT("\t.CompilerOutputExtension = '.fbout'\r\n")
					TEXT("\t.CompilerInputFiles = { '%s' }\r\n")
					TEXT("\t.CompilerOutputPath = '%s'\r\n")
					TEXT("}\r\n\r\n"),
					Batch->BatchIndex,
					Manager->ProcessId,
					Batch->BatchIndex,
					*FCommandLine::GetSubprocessCommandline(),
					*Batch->InputFileNameAndPath,
					*WorkerAbsoluteDirectory);

				//FString TaskXML = FString::Printf(TEXT("\t\t\t<Task Caption=\"Compiling %d Shaders (Batch %d)\" Params=\"%s\" />\r\n"), Batch->NumJobs(), Batch->BatchIndex, *WorkerParameters);

				ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*ExecFunction, ExecFunction.Len()).Get(), sizeof(ANSICHAR) * ExecFunction.Len());
			}


			FString AliasBuildTargetOpen = FString(
				TEXT("Alias('all')\r\n")
				TEXT("{\r\n")
				TEXT("\t.Targets = { \r\n")
			);
			ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*AliasBuildTargetOpen, AliasBuildTargetOpen.Len()).Get(), sizeof(ANSICHAR) * AliasBuildTargetOpen.Len());

			// Write write the "All" target
			for (FShaderBatch* Batch : ShaderBatchesInFlight) {
				FString TargetExport = FString::Printf(TEXT("'ShaderBatch-%d', "), Batch->BatchIndex);

				ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*TargetExport, TargetExport.Len()).Get(), sizeof(ANSICHAR) * TargetExport.Len());
			}

			FString AliasBuildTargetClose = FString(TEXT(" }\r\n}\r\n"));
			ScriptFile->Serialize((void*)StringCast<ANSICHAR>(*AliasBuildTargetClose, AliasBuildTargetClose.Len()).Get(), sizeof(ANSICHAR) * AliasBuildTargetClose.Len());

			// End the XML script file and close it.
			WriteFASTBuildScriptFileFooter(ScriptFile);
			delete ScriptFile;
			ScriptFile = nullptr;

			// Grab the timestamp from the script file.
			// We use this to ignore any left over files from previous builds by only accepting files created after the script file.
			ScriptFileCreationTime = IFileManager::Get().GetTimeStamp(*ScriptFilename);

			StartTime = FPlatformTime::Cycles();

			// Use stop on errors so we can respond to shader compile worker crashes immediately.
			// Regular shader compilation errors are not returned as worker errors.
			//-forceremote
			//FString FBConsoleArgs = TEXT("-config \"") + ScriptFilename + TEXT("\" -dist -monitor -cache");
			FString FBConsoleArgs = TEXT("-config \"") + ScriptFilename + TEXT("\" -monitor -summary");
			if(!FASTBuildConsoleVariables::bDisableRemoteCompile){
				FBConsoleArgs += TEXT(" -dist ");
				if (FASTBuildConsoleVariables::bForceRemoteCompile) {
					FBConsoleArgs += TEXT(" -forceremote ");
				}
			}
			if (!FASTBuildConsoleVariables::bDisableCache) {
				FBConsoleArgs += TEXT(" -cache ");
			}

			UE_LOG(LogShaderCompilers, Warning, TEXT("fast build running cmd:%s %s"), *GetFASTBuild_ExecutablePath(), *FBConsoleArgs);
			// Kick off the FASTBuild process...
			//PipeWriteChild=c:/tmp/fbuild.log

			void* PipeRead = nullptr;
			void* PipeWrite = nullptr;

			//verify(FPlatformProcess::CreatePipe(PipeRead, PipeWrite));
			//BuildProcessHandle = FPlatformProcess::CreateProc(*GetFASTBuild_ExecutablePath(), *FBConsoleArgs, false, false, true, &BuildProcessID, 0, nullptr, PipeWrite, nullptr);
			BuildProcessHandle = FPlatformProcess::CreateProc(*GetFASTBuild_ExecutablePath(), *FBConsoleArgs, false, false, true, &BuildProcessID, 0, nullptr, PipeWrite, nullptr);
			if (!BuildProcessHandle.IsValid()) {
				UE_LOG(LogShaderCompilers, Fatal, TEXT("Failed to launch %s during shader compilation."), *GetFASTBuild_ExecutablePath());
			}
			else {
				/*
				//debug 
				FString OutProcOutput;
				float maxWaitTime = 30.0f;
				while (FPlatformProcess::IsProcRunning(BuildProcessHandle)) {
					OutProcOutput += FPlatformProcess::ReadPipe(PipeRead);
					FPlatformProcess::Sleep(0.1f);
					maxWaitTime -= 0.1f;
				}
				UE_LOG(LogShaderCompilers, Log, TEXT("%s"), *OutProcOutput);
				int32_t dwCode = 0;
				FPlatformProcess::GetProcReturnCode(BuildProcessHandle, &dwCode);
				UE_LOG(LogShaderCompilers, Log, TEXT("Check Output Process Id:%d Ret:%d"), BuildProcessHandle.Get(), dwCode);
				//FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
				*/
			}


			// If the engine crashes, we don't get a chance to kill the build process.
			// Start up the build monitor process to monitor for engine crashes.
			uint32 BuildMonitorProcessID;
			FProcHandle BuildMonitorHandle = FPlatformProcess::CreateProc(*Manager->ShaderCompileWorkerName, *FString::Printf(TEXT("-xgemonitor %d %d"), Manager->ProcessId, BuildProcessID), true, false, false, &BuildMonitorProcessID, 0, nullptr, nullptr);
			FPlatformProcess::CloseProc(BuildMonitorHandle);

			// Reset batch counters and switch directories
			BatchIndexToFill = 0;
			BatchIndexToCreate = 0;
			FASTBuildDirectoryIndex = 1 - FASTBuildDirectoryIndex;

			bWorkRemaining = true;
		}
	}

	// Try to prepare more shader jobs (even if a build is in flight).
	TArray<FShaderCommonCompileJob*> JobQueue;
	{
		// Enter the critical section so we can access the input and output queues
		FScopeLock Lock(&Manager->CompileQueueSection);

		// Grab as many jobs from the job queue as we can.
		int32 NumNewJobs = Manager->CompileQueue.Num();
		if (NumNewJobs > 0) {
			int32 DestJobIndex = JobQueue.AddUninitialized(NumNewJobs);
			for (int32 SrcJobIndex = 0; SrcJobIndex < NumNewJobs; SrcJobIndex++, DestJobIndex++) {
				JobQueue[DestJobIndex] = Manager->CompileQueue[SrcJobIndex];
			}

			Manager->CompileQueue.RemoveAt(0, NumNewJobs);
		}
	}

	if (JobQueue.Num() > 0) {
		// We have new jobs in the queue.
		// Group the jobs into batches and create the worker input files.
		for (int32 JobIndex = 0; JobIndex < JobQueue.Num(); JobIndex++) {
			if (BatchIndexToFill >= ShaderBatchesIncomplete.GetMaxIndex() || !ShaderBatchesIncomplete.IsAllocated(BatchIndexToFill)) {
				// There are no more incomplete shader batches available.
				// Create another one...
				ShaderBatchesIncomplete.Insert(BatchIndexToFill, new FShaderBatch(
					FASTBuildWorkingDirectory,
					FASTBuild_InputFileName,
					FASTBuild_SuccessFileName,
					FASTBuild_OutputFileName,
					FASTBuildDirectoryIndex,
					BatchIndexToCreate));

				BatchIndexToCreate++;
			}

			// Add a single job to this batch
			FShaderBatch* CurrentBatch = ShaderBatchesIncomplete[BatchIndexToFill];
			CurrentBatch->AddJob(JobQueue[JobIndex]);

			// If the batch is now full...
			if (CurrentBatch->NumJobs() == FASTBuildConsoleVariables::BatchSize) {
				CurrentBatch->WriteTransferFile();

				// Move the batch to the full list.
				ShaderBatchesFull.Add(CurrentBatch);
				ShaderBatchesIncomplete.RemoveAt(BatchIndexToFill);
			}

			BatchIndexToFill++;
			BatchIndexToFill %= FASTBuildConsoleVariables::BatchGroupSize;
		}

		// Keep track of the last time we added jobs.
		LastAddTime = FPlatformTime::Cycles();

		bWorkRemaining = true;
	}

	if (Manager->bAllowAsynchronousShaderCompiling) {
		// Yield for a short while to stop this thread continuously polling the disk.
		FPlatformProcess::Sleep(0.01f);
	}

	return bWorkRemaining ? 1 : 0;
}

#endif
