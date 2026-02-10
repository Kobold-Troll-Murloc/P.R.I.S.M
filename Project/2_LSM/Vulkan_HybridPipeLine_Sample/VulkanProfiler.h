#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip> // std::fixed, std::setprecision

struct TimeStamp {
    std::string name;
    uint32_t startQueryIdx;
    uint32_t endQueryIdx;
};

class VulkanProfiler {
public:
    // 초기화: Vulkan Device와 Physical Device가 필요합니다.
    void init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t maxQueries = 128) {
        this->device = device;
        this->maxQueries = maxQueries;

        // 1. 타임스탬프 주기(Period) 가져오기 (나노초 -> 밀리초 변환용)
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        timestampPeriod = props.limits.timestampPeriod;

        // 2. 쿼리 풀(Query Pool) 생성
        VkQueryPoolCreateInfo queryPoolInfo{};
        queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryPoolInfo.queryCount = maxQueries;

        if (vkCreateQueryPool(device, &queryPoolInfo, nullptr, &queryPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create query pool!");
        }
    }

    // 종료 시 리소스 해제
    void cleanup() {
        if (queryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, queryPool, nullptr);
        }
    }

    // 프레임 시작 시 호출 (커맨드 버퍼 기록 시작 직후)
    void beginFrame(VkCommandBuffer cmdBuf) {
        vkCmdResetQueryPool(cmdBuf, queryPool, 0, maxQueries);
        currentQueryIdx = 0;
        timeStamps.clear();
    }

    // 구간 측정 시작 (예: "Raster Pass")
    void beginSection(VkCommandBuffer cmdBuf, const std::string& name) {
        if (currentQueryIdx + 2 > maxQueries) return; // 쿼리 풀 초과 방지

        uint32_t idx = currentQueryIdx++;
        // 파이프라인의 가장 앞단(Top)에서 타임스탬프 기록
        vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, idx);

        TimeStamp ts;
        ts.name = name;
        ts.startQueryIdx = idx;
        timeStamps.push_back(ts);
    }

    // 구간 측정 종료
    void endSection(VkCommandBuffer cmdBuf) {
        if (timeStamps.empty()) return;
        if (currentQueryIdx >= maxQueries) return;

        TimeStamp& ts = timeStamps.back();
        uint32_t idx = currentQueryIdx++;
        ts.endQueryIdx = idx;

        // 파이프라인의 가장 뒷단(Bottom)에서 타임스탬프 기록
        vkCmdWriteTimestamp(cmdBuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, idx);
    }

    // [옵션 1] 콘솔창에 결과 출력 (vkQueueWaitIdle 필요)
    void printResults() {
        if (currentQueryIdx == 0) return;

        std::vector<uint64_t> buffer(currentQueryIdx);

        // GPU가 값을 다 쓸 때까지 기다렸다가 가져옴 (WAIT_BIT)
        VkResult result = vkGetQueryPoolResults(device, queryPool, 0, currentQueryIdx,
            sizeof(uint64_t) * currentQueryIdx, buffer.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        if (result == VK_SUCCESS) {
            std::cout << "\n=== Performance Frame ===" << std::endl;
            float totalGpuTime = 0.0f;

            for (const auto& ts : timeStamps) {
                uint64_t start = buffer[ts.startQueryIdx];
                uint64_t end = buffer[ts.endQueryIdx];

                // 나노초 -> 밀리초 변환 (timestampPeriod는 GPU마다 다름)
                float durationMs = (end - start) * timestampPeriod / 1000000.0f;

                std::cout << "[GPU] " << ts.name << ": " << durationMs << " ms" << std::endl;
                totalGpuTime += durationMs;
            }
            std::cout << "-------------------------" << std::endl;
            std::cout << "Total GPU Time: " << totalGpuTime << " ms" << std::endl;
            if (totalGpuTime > 0)
                std::cout << "Est. FPS: " << (1000.0f / totalGpuTime) << std::endl;
        }
    }

    // [옵션 2] 결과를 문자열로 반환 (윈도우 타이틀바 표시용, Non-Blocking)
    // WaitIdle 없이 즉시 반환을 시도하므로, 렌더링을 멈추지 않고 쓸 수 있습니다.
    std::string getResultsString() {
        if (currentQueryIdx == 0) return "Profiling...";

        std::vector<uint64_t> buffer(currentQueryIdx);
        // VK_QUERY_RESULT_WAIT_BIT를 뺐습니다. 아직 GPU가 일하는 중이면 VK_NOT_READY를 반환합니다.
        VkResult result = vkGetQueryPoolResults(device, queryPool, 0, currentQueryIdx,
            sizeof(uint64_t) * currentQueryIdx, buffer.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

        if (result != VK_SUCCESS) return "Collecting...";

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);

        float totalGpuTime = 0.0f;
        ss << "[GPU] ";

        for (size_t i = 0; i < timeStamps.size(); ++i) {
            const auto& ts = timeStamps[i];
            uint64_t start = buffer[ts.startQueryIdx];
            uint64_t end = buffer[ts.endQueryIdx];
            float durationMs = (end - start) * timestampPeriod / 1000000.0f;

            ss << ts.name << ": " << durationMs << "ms";
            if (i < timeStamps.size() - 1) ss << " | ";

            totalGpuTime += durationMs;
        }

        if (totalGpuTime > 0) {
            ss << " | Total: " << totalGpuTime << "ms (" << (int)(1000.0f / totalGpuTime) << " FPS)";
        }

        return ss.str();
    }

private:
    VkDevice device = VK_NULL_HANDLE;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 1.0f;
    uint32_t maxQueries = 0;
    uint32_t currentQueryIdx = 0;
    std::vector<TimeStamp> timeStamps;
};