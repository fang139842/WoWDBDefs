#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoWEditorModule, Log, All);

class FWoWEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogWoWEditorModule, Display, TEXT("wowEditor module loaded"));
	}
};

IMPLEMENT_MODULE(FWoWEditorModule, wowEditor)
