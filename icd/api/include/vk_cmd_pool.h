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
 * @file  vk_cmd_pool.h
 * @brief Declaration of Vulkan command buffer pool class.
 **************************************************************************************************
 */

#ifndef __VK_CMD_POOL_H__
#define __VK_CMD_POOL_H__

#pragma once

#include "include/khronos/vulkan.h"
#include "include/vk_device.h"
#include "include/vk_dispatch.h"
#include "include/vk_alloccb.h"

#include "palCmdAllocator.h"
#include "palHashSet.h"

namespace vk
{

class Device;
class CmdBuffer;

// =====================================================================================================================
// A Vulkan command buffer pool
class CmdPool final : public NonDispatchable<VkCommandPool, CmdPool>
{
public:
    static VkResult Create(
        Device*                         pDevice,
        const VkCommandPoolCreateInfo*  pCreateInfo,
        const VkAllocationCallbacks*    pAllocator,
        VkCommandPool*                  pCmdPool);

    VkResult Init();

    VkResult Destroy(
        Device*                         pDevice,
        const VkAllocationCallbacks*    pAllocator);

    VkResult Reset(VkCommandPoolResetFlags flags);

    Pal::ICmdAllocator* PalCmdAllocator(int32_t idx)
    {
        VK_ASSERT((idx >= 0) && (idx < static_cast<int32_t>(m_pDevice->NumPalDevices())));
        return m_pPalCmdAllocators[idx];
    }

    Pal::Result RegisterCmdBuffer(CmdBuffer* pCmdBuffer);

    void UnregisterCmdBuffer(CmdBuffer* pCmdBuffer);

    VK_INLINE uint32_t GetQueueFamilyIndex() const { return m_queueFamilyIndex; }

    const VkAllocationCallbacks* GetCmdPoolAllocator() const { return m_pAllocator; }

    bool IsProtected() const { return m_flags.isProtected ? true : false; }

    // Marks command buffer as needing an explicit reset when this cmd
    // pool is reset.
    Pal::Result MarkExplicitlyResetCmdBuf(CmdBuffer* pCmdBuffer);

    // Removes `pCmdBuffer` from the set of command buffers to reset
    // explicitly.
    void UnmarkExplicitlyResetCmdBuf(CmdBuffer* pCmdBuffer);

private:
    PAL_DISALLOW_COPY_AND_ASSIGN(CmdPool);

    CmdPool(
        Device*                      pDevice,
        Pal::ICmdAllocator**         pPalCmdAllocators,
        const VkAllocationCallbacks* pAllocator,
        uint32_t                     queueFamilyIndex,
        VkCommandPoolCreateFlags     flags,
        bool                         sharedCmdAllocator);

    VkResult ResetCmdAllocator();

    Device*                      m_pDevice;
    Pal::ICmdAllocator*          m_pPalCmdAllocators[MaxPalDevices];
    const VkAllocationCallbacks* m_pAllocator;
    const uint32_t               m_queueFamilyIndex;
    const bool                   m_sharedCmdAllocator;

    union
    {
        struct
        {
            uint32 isProtected : 1;
            uint32 reserved    : 31;
        };
        uint32 u32All;
    } m_flags;

    Util::HashSet<CmdBuffer*, PalAllocator> m_cmdBufferRegistry;

    // Command buffes that need to be explicitly reset on cmd pool reset.
    Util::HashSet<CmdBuffer*, PalAllocator> m_cmdBufsForExplicitReset;
    // Indicates that the command pool is currently being reset.
    bool m_poolResetInProgress = false;
};

namespace entry
{

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    VkCommandPoolResetFlags                     flags);
} // namespace entry

} // namespace vk

#endif /* __VK_CMD_POOL_H__ */
