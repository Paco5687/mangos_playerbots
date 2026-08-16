#include <atomic>
#include <string>
#include <vector>

class PlayerbotLLMInterface
{
public:
    PlayerbotLLMInterface() {}
    static std::string SanitizeForJson(const std::string& input);

    static std::string Generate(const std::string& prompt, int timeOutSeconds, int maxGenerations, std::vector<std::string>& debugLines);

    // Minimal plain-http POST for local bridges (the Storyteller, issue #59).
    // No SSL, no generation-slot accounting - a localhost roundtrip must not
    // compete with the model's concurrency budget. Returns the response body,
    // or "" on any failure.
    static std::string Post(const std::string& url, const std::string& body,
                            const std::string& apiToken, int timeOutSeconds);

    static std::vector<std::string> ParseResponse(const std::string& response, const std::string& startPattern, const std::string& endPattern, const std::string& deletePattern, const std::string& splitPattern, std::vector<std::string>& debugLines);

    static void LimitContext(std::string& context, int currentLength);
private:
    std::atomic<int> generationCount = 0;
};

#define sPlayerbotLLMInterface MaNGOS::Singleton<PlayerbotLLMInterface>::Instance()

