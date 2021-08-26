/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2014-2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 **************************************************************************************************
 * @file  vk_cmd_pool.cpp
 * @brief Implementation of Vulkan command buffer pool class.
 **************************************************************************************************
 */

#include "include/vk_cmd_pool.h"
#include "include/vk_cmdbuffer.h"
#include "include/vk_instance.h"
#include "include/vk_device.h"
#include "include/vk_conv.h"

#include "palFile.h"
#include "palHashSetImpl.h"
#include "palIntrusiveListImpl.h"
#include "palVectorImpl.h"

#if ICD_GPUOPEN_DEVMODE_BUILD
#include "devmode/devmode_mgr.h"
#endif

namespace vk
{

// =====================================================================================================================
CmdPool::CmdPool(
    Device*                      pDevice,
    Pal::ICmdAllocator**         pPalCmdAllocators,
    const VkAllocationCallbacks* pAllocator,
    uint32_t                     queueFamilyIndex,
    VkCommandPoolCreateFlags     flags,
    bool                         sharedCmdAllocator)
    :
    m_pDevice(pDevice),
    m_pAllocator(pAllocator),
    m_queueFamilyIndex(queueFamilyIndex),
    m_sharedCmdAllocator(sharedCmdAllocator),
    m_cmdBufferRegistry(32, pDevice->VkInstance()->Allocator()),
    m_cmdBufsForExplicitReset(32, pDevice->VkInstance()->Allocator()),
    m_palDepthStencilStates(32, pDevice->VkInstance()->Allocator()),
    m_resettableInstanceAllocs(32, pDevice->VkInstance()->Allocator()),
    m_stackAllocators(32, pDevice->VkInstance()->Allocator())
{
    m_flags.u32All = 0;

    if (flags & VK_COMMAND_POOL_CREATE_PROTECTED_BIT)
    {
        m_flags.isProtected = true;
    }

    const RuntimeSettings& settings = m_pDevice->GetRuntimeSettings();
    m_flags.disableResetReleaseResources = settings.disableResetReleaseResources;

    memcpy(m_pPalCmdAllocators, pPalCmdAllocators, sizeof(pPalCmdAllocators[0]) * pDevice->NumPalDevices());
}

// =====================================================================================================================
// Initializes the command buffer pool object.
VkResult CmdPool::Init()
{
    Pal::Result palResult = m_cmdBufferRegistry.Init();

    if (palResult == Pal::Result::Success)
    {
        palResult = m_cmdBufsForExplicitReset.Init();
    }

    if (palResult == Pal::Result::Success)
    {
        palResult = m_palDepthStencilStates.Init();
    }

    if (palResult == Pal::Result::Success)
    {
        palResult = m_resettableInstanceAllocs.Init();
    }

    if (palResult == Pal::Result::Success)
    {
        palResult = m_stackAllocators.Init();
    }

    return PalToVkResult(palResult);
}

// =====================================================================================================================
VkResult CmdPool::Create(
    Device*                        pDevice,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkCommandPool*                 pCmdPool)
{
    const RuntimeSettings* pSettings = &pDevice->VkPhysicalDevice(DefaultDeviceIndex)->GetRuntimeSettings();

    void* pMemory   = nullptr;
    VkResult result = VK_SUCCESS;

    Pal::ICmdAllocator* pPalCmdAllocator[MaxPalDevices] = {};

    if (pSettings->useSharedCmdAllocator)
    {
        // Use the per-device shared CmdAllocator if the settings indicate so.
        for (uint32_t deviceIdx = 0; deviceIdx < pDevice->NumPalDevices(); deviceIdx++)
        {
            pPalCmdAllocator[deviceIdx] = pDevice->GetSharedCmdAllocator(deviceIdx);
        }

        pMemory = pDevice->AllocApiObject(pAllocator, sizeof(CmdPool));

        if (pMemory == nullptr)
        {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    else
    {
        // Create a private CmdAllocator for this command buffer pool. As the application can only use a CmdPool
        // object in a single thread at any given time, we don't need a thread safe CmdAllocator.
        Pal::CmdAllocatorCreateInfo createInfo = { };

        createInfo.flags.autoMemoryReuse          = 1;
        createInfo.flags.disableBusyChunkTracking = 1;

        // Initialize command data chunk allocation size
        createInfo.allocInfo[Pal::CommandDataAlloc].allocHeap    = pSettings->cmdAllocatorDataHeap;
        createInfo.allocInfo[Pal::CommandDataAlloc].allocSize    = pSettings->cmdAllocatorDataAllocSize;
        createInfo.allocInfo[Pal::CommandDataAlloc].suballocSize = pSettings->cmdAllocatorDataSubAllocSize;

        // Initialize embedded data chunk allocation size
        createInfo.allocInfo[Pal::EmbeddedDataAlloc].allocHeap    = pSettings->cmdAllocatorEmbeddedHeap;
        createInfo.allocInfo[Pal::EmbeddedDataAlloc].allocSize    = pSettings->cmdAllocatorEmbeddedAllocSize;
        createInfo.allocInfo[Pal::EmbeddedDataAlloc].suballocSize = pSettings->cmdAllocatorEmbeddedSubAllocSize;

        // Initialize GPU scratch memory chunk allocation size
        createInfo.allocInfo[Pal::GpuScratchMemAlloc].allocHeap    = pSettings->cmdAllocatorScratchHeap;
        createInfo.allocInfo[Pal::GpuScratchMemAlloc].allocSize    = pSettings->cmdAllocatorScratchAllocSize;
        createInfo.allocInfo[Pal::GpuScratchMemAlloc].suballocSize = pSettings->cmdAllocatorScratchSubAllocSize;

        Pal::Result  palResult     = Pal::Result::Success;
        const size_t allocatorSize = pDevice->PalDevice(DefaultDeviceIndex)->GetCmdAllocatorSize(createInfo, &palResult);

        if (palResult == Pal::Result::Success)
        {
            size_t apiSize = sizeof(CmdPool);
            size_t palSize = allocatorSize * pDevice->NumPalDevices();

            pMemory = pDevice->AllocApiObject(pAllocator, apiSize + palSize);

            if (pMemory != NULL)
            {
                void* pAllocatorMem = Util::VoidPtrInc(pMemory, apiSize);

                for (uint32_t deviceIdx = 0;
                    (deviceIdx < pDevice->NumPalDevices()) && (palResult == Pal::Result::Success);
                    deviceIdx++)
                {
                    palResult = pDevice->PalDevice(deviceIdx)->CreateCmdAllocator(
                        createInfo,
                        Util::VoidPtrInc(pAllocatorMem, allocatorSize * deviceIdx),
                        &pPalCmdAllocator[deviceIdx]);
                }

                result = PalToVkResult(palResult);

                if (result != VK_SUCCESS)
                {
                    pDevice->FreeApiObject(pAllocator, pMemory);
                    pMemory = nullptr;
                }
            }
            else
            {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        else
        {
            result = PalToVkResult(palResult);
        }
    }

    if (result == VK_SUCCESS)
    {
        VK_PLACEMENT_NEW(pMemory) CmdPool(
            pDevice,
            pPalCmdAllocator,
            pAllocator,
            pCreateInfo->queueFamilyIndex,
            pCreateInfo->flags,
            pSettings->useSharedCmdAllocator);

        VkCommandPool handle = CmdPool::HandleFromVoidPointer(pMemory);
        CmdPool* pApiCmdPool = CmdPool::ObjectFromHandle(handle);

        result = pApiCmdPool->Init();

        if (result == VK_SUCCESS)
        {
            *pCmdPool = handle;
        }
        else
        {
            pApiCmdPool->Destroy(pDevice, pAllocator);
        }
    }

    return result;
}

// =====================================================================================================================
// Destroy a command buffer pool object
VkResult CmdPool::Destroy(
    Device*                         pDevice,
    const VkAllocationCallbacks*    pAllocator)
{
    // When a command pool is destroyed, all command buffers allocated from the pool are implicitly freed and
    // become invalid.
    while (m_cmdBufferRegistry.GetNumEntries() > 0)
    {
        CmdBuffer* pCmdBuf = m_cmdBufferRegistry.Begin().Get()->key;

        pCmdBuf->Destroy();
    }

    // If we don't use a shared CmdAllocator then we have to destroy our own one.
    if (m_sharedCmdAllocator == false)
    {
        for (uint32_t deviceIdx = 0; deviceIdx < pDevice->NumPalDevices(); deviceIdx++)
        {
            m_pPalCmdAllocators[deviceIdx]->Destroy();
        }
    }

    Util::Destructor(this);

    pDevice->FreeApiObject(pAllocator, this);

    return VK_SUCCESS;
}

// =====================================================================================================================
// Resets the PAL command allocators
VkResult CmdPool::ResetCmdAllocator()
{
    Pal::Result result = Pal::Result::Success;

    for (uint32_t deviceIdx = 0;
        (deviceIdx < m_pDevice->NumPalDevices()) && (result == Pal::Result::Success);
        deviceIdx++)
    {
        result = m_pPalCmdAllocators[deviceIdx]->Reset();
    }

    return PalToVkResult(result);
}

// =====================================================================================================================
// Reset a command buffer pool object
VkResult CmdPool::Reset(VkCommandPoolResetFlags flags)
{
    VkResult result = VK_SUCCESS;

    m_poolResetInProgress = true;

    // There's currently no way to tell to the PAL CmdAllocator that it should release the actual allocations used
    // by the pool, it always just marks the allocations unused, so we currently ignore the
    // VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT flag if present.
    VK_IGNORE(flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
    bool needToReleaseResources = flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT;
    if (m_flags.disableResetReleaseResources)
    {
        needToReleaseResources = false;
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 675 // TODO: Use the actual number when deferred reset is supported by PAL.
    // We may only use deferred PAL cmd buffer reset when the allocator is reset
    // here because we rely on allocator reset.
    const bool deferPalCmdBufferReset = !m_sharedCmdAllocator;
#else
    const bool deferPalCmdBufferReset = false;
#endif

    auto& bufsToReset = deferPalCmdBufferReset ? m_cmdBufsForExplicitReset : m_cmdBufferRegistry;

    // We first have to reset all the command buffers that are marked for explicit reset (PAL doesn't do this automatically).
    for (auto it = bufsToReset.Begin(); (it.Get() != nullptr) && (result == VK_SUCCESS); it.Next())
    {
        // Per-spec we always have to do a command buffer reset that also releases the used resources.
        result = it.Get()->key->Reset(VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    }

    if (result == VK_SUCCESS)
    {
        // Clear the set of command buffers to reset. Only done if all the
        // buffers were reset successfully so it is possible that after an error
        // this set will contain already reset command buffers. This is fine
        // because we can reset command buffers twice.
        if (m_cmdBufsForExplicitReset.GetNumEntries() > 0)
        {
            m_cmdBufsForExplicitReset.Reset();
        }

        if (needToReleaseResources)
        {
            // Release resources that are being held by the command buffers.
            // The CBs will synchronize their state later either on begin or free.
            // Resource resets are gated by GetNumEntries() because resetting
            // empty hashsets is not free, although in this instance the difference
            // is pretty small.

            if (m_palDepthStencilStates.GetNumEntries() > 0)
            {
                RenderStateCache* pRSCache = m_pDevice->GetRenderStateCache();
                for (auto it = m_palDepthStencilStates.Begin(); it.Get() != nullptr; it.Next())
                {
                    // Undo each state creation performed by cmd buffers in the pool.
                    for (uint32 i = 0; i < it.Get()->value; ++i)
                    {
                        pRSCache->DestroyDepthStencilState(it.Get()->key,
                                                           m_pDevice->VkInstance()->GetAllocCallbacks());
                    }
                }
                m_palDepthStencilStates.Reset();
            }

            if (m_resettableInstanceAllocs.GetNumEntries() > 0)
            {
                for (auto it = m_resettableInstanceAllocs.Begin(); it.Get() != nullptr; it.Next())
                {
                    m_pDevice->VkInstance()->FreeMem(it.Get()->key);
                }
                m_resettableInstanceAllocs.Reset();
            }

            if (m_stackAllocators.GetNumEntries() > 0)
            {
                for (auto it = m_stackAllocators.Begin(); it.Get() != nullptr; it.Next())
                {
                    m_pDevice->VkInstance()->StackMgr()->ReleaseAllocator(it.Get()->key);
                }
                m_stackAllocators.Reset();
            }

            ++m_instanceResourceGeneration;
        }

        // After resetting the registered command buffers, reset the pool itself but only if we use per-pool
        // CmdAllocator objects, not a single shared one.
        if (m_sharedCmdAllocator == false)
        {
            result = ResetCmdAllocator();
        }
    }

    m_poolResetInProgress = false;

    return result;
}

// =====================================================================================================================
// Register a command buffer with this pool. Used to reset the command buffers at pool reset time.
Pal::Result CmdPool::RegisterCmdBuffer(CmdBuffer* pCmdBuffer)
{
    return m_cmdBufferRegistry.Insert(pCmdBuffer);
}

// =====================================================================================================================
// Unregister a command buffer from this pool.
void CmdPool::UnregisterCmdBuffer(CmdBuffer* pCmdBuffer)
{
    // Remove the buffer from the list of explicitly reset buffers if
    // needed.
    UnmarkExplicitlyResetCmdBuf(pCmdBuffer);
    m_cmdBufferRegistry.Erase(pCmdBuffer);
}

// =====================================================================================================================
// Marks command buffer as needing an explicit reset when this cmd
// pool is reset.
Pal::Result CmdPool::MarkExplicitlyResetCmdBuf(CmdBuffer* pCmdBuffer)
{
    // If a reset is in progress we can't update the list of cmd buffers to
    // reset.
    VK_ASSERT(!m_poolResetInProgress);
    return m_cmdBufsForExplicitReset.Insert(pCmdBuffer);
}

// =====================================================================================================================
// Removes `pCmdBuffer` from the set of command buffers to reset
// explicitly.
void CmdPool::UnmarkExplicitlyResetCmdBuf(CmdBuffer* pCmdBuffer)
{
    // If a reset is in progress we can't update the list of command buffers to
    // reset because it may be used. It is safe to ignore this operation since
    // command pool objects are externally synchronized, so
    // UnmarkExplicitlyResetCmdBuf can only be called from CmdBuffer::End() done
    // as a part of the pool reset.
    if (m_poolResetInProgress)
    {
        return;
    }
    m_cmdBufsForExplicitReset.Erase(pCmdBuffer);
}

// =====================================================================================================================
// Forwards the call to RenderStateCache saving the output to
// the internal set with resouces to be returned on Reset().
Pal::Result CmdPool::CreateDepthStencilState(
    const Pal::DepthStencilStateCreateInfo& createInfo,
    VkSystemAllocationScope                 parentScope,
    Pal::IDepthStencilState*                pStates[MaxPalDevices])
{
    RenderStateCache* pRSCache = m_pDevice->GetRenderStateCache();
    Pal::Result palResult = pRSCache->CreateDepthStencilState(createInfo,
                                                              m_pDevice->VkInstance()->GetAllocCallbacks(),
                                                              parentScope,
                                                              pStates);

    if (palResult == Pal::Result::Success)
    {
        bool existed = false;
        uint32* pCount = nullptr;
        palResult = m_palDepthStencilStates.FindAllocate(pStates, &existed, &pCount);
        if (palResult == Pal::Result::Success)
        {
            if (existed)
            {
                ++(*pCount);
            }
            else
            {
                *pCount = 1;
            }
        }
    }
    return palResult;
}

// =====================================================================================================================
// Forwards the call to RenderStateCache and removes `ppStates` from
// the tracked set.
void CmdPool::DestroyDepthStencilState(Pal::IDepthStencilState** ppStates)
{
    RenderStateCache* pRSCache = m_pDevice->GetRenderStateCache();
    pRSCache->DestroyDepthStencilState(ppStates,
                                       m_pDevice->VkInstance()->GetAllocCallbacks());
    uint32* pCount = m_palDepthStencilStates.FindKey(ppStates);
    PAL_ASSERT(pCount);
    PAL_ASSERT(*pCount > 0);
    if (*pCount > 0)
    {
        --(*pCount);
    }
    if (*pCount == 0)
    {
        m_palDepthStencilStates.Erase(ppStates);
    }
}


// =====================================================================================================================
// Allocates instance memory saving the output to the tracked set
// with resouces to be returned on Reset().
void* CmdPool::AllocMem(size_t size, VkSystemAllocationScope allocType)
{
    void* pMem = m_pDevice->VkInstance()->AllocMem(size, allocType);
    if (pMem != nullptr)
    {
        m_resettableInstanceAllocs.Insert(pMem);
    }
    return pMem;
}

// =====================================================================================================================
// Frees instance memory and removes `pMem` from the tracked set.
void CmdPool::FreeMem(void* pMem)
{
    m_pDevice->VkInstance()->FreeMem(pMem);
    if (pMem != nullptr)
    {
        PAL_ASSERT(m_resettableInstanceAllocs.Contains(pMem));
        m_resettableInstanceAllocs.Erase(pMem);
    }
}

// =====================================================================================================================
// Acquires a virtual stack alocator from the instance saving the
// output to the tracked set with resouces to be returned on Reset().
Pal::Result CmdPool::AcquireAllocator(VirtualStackAllocator** ppAllocator)
{
    Pal::Result palResult = m_pDevice->VkInstance()->StackMgr()->AcquireAllocator(ppAllocator);
    if (palResult == Pal::Result::Success)
    {
        palResult = m_stackAllocators.Insert(*ppAllocator);
    }
    return palResult;
}

// =====================================================================================================================
// Releases `pAllocator` and removes it from the tracked set.
void CmdPool::ReleaseAllocator(VirtualStackAllocator* pAllocator)
{
    PAL_ASSERT(m_stackAllocators.Contains(pAllocator));
    m_pDevice->VkInstance()->StackMgr()->ReleaseAllocator(pAllocator);
    m_stackAllocators.Erase(pAllocator);
}

/**
 ***********************************************************************************************************************
 * C-Callable entry points start here. These entries go in the dispatch table(s).
 ***********************************************************************************************************************
 */

namespace entry
{

// =====================================================================================================================

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    const VkAllocationCallbacks*                pAllocator)
{
    if (commandPool != VK_NULL_HANDLE)
    {
        Device*                      pDevice  = ApiDevice::ObjectFromHandle(device);
        const VkAllocationCallbacks* pAllocCB = pAllocator ? pAllocator : pDevice->VkInstance()->GetAllocCallbacks();

        CmdPool::ObjectFromHandle(commandPool)->Destroy(pDevice, pAllocCB);
    }
}

// =====================================================================================================================
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    VkCommandPoolResetFlags                     flags)
{
    return CmdPool::ObjectFromHandle(commandPool)->Reset(flags);
}

} // namespace entry

} // namespace vk
