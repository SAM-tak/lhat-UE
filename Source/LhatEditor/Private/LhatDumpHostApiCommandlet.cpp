#include "LhatDumpHostApiCommandlet.h"

#include "LhatHostApiExport.h"
#include "LhatModule.h"
#include "Misc/Parse.h"

ULhatDumpHostApiCommandlet::ULhatDumpHostApiCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 ULhatDumpHostApiCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params, TEXT("Help")))
	{
		UE_LOG(LogLhat, Display, TEXT("-run=LhatDumpHostApi [-Output=\"path\"] [-BindingReport]. Defaults: <Project>/lhat-host.json or lhat-bindings.json for the dispatch/exclusion report. Existing output is replaced."));
		return 0;
	}
	FString RequestedPath;
	const bool bHasOutput = FParse::Value(*Params, TEXT("Output="), RequestedPath);
	if ((bHasOutput && RequestedPath.IsEmpty()) || FParse::Param(*Params, TEXT("Output")))
	{
		UE_LOG(LogLhat, Error, TEXT("Output requires a nonempty path: -Output=\"path\"."));
		return 2;
	}
	FString OutputFile, Error;
	if (!LhatHostApi::Export(RequestedPath, OutputFile, Error, FParse::Param(*Params, TEXT("BindingReport"))))
	{
		UE_LOG(LogLhat, Error, TEXT("Host API export failed: %s"), *Error);
		return 1;
	}
	UE_LOG(LogLhat, Display, TEXT("Host API written to %s"), *OutputFile);
	return 0;
}
