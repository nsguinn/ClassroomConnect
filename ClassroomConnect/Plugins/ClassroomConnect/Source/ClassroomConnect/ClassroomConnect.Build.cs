using UnrealBuildTool;
public class ClassroomConnect : ModuleRules
{
    public ClassroomConnect(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "MainFrame", "Slate", "SlateCore", "ToolMenus", "Projects", "Json", "Messaging", "MessagingCommon",
            // Concert public APIs verified against the installed 5.8.4 headers; ConcertSyncClient re-exports Concert and ConcertClient.
            "MultiUserClient", "ConcertSyncClient", "ConcertClient", "Concert", "ConcertTransport" });
        // Streaming SHA-256 for verifying copied project files (Windows CNG).
        PublicSystemLibraries.Add("bcrypt.lib");
    }
}
