// Runs a vertex shader (SPIR-V dumped with RT_VK_DUMP) on the bundled KosmicKrisp with a vertex layout given on
// the command line, for one vertex, and prints the vertex outputs (captured by a fragment shader that writes the
// chosen output location to a 1x1 RGBA32F target). Uniform buffers at set 0 bindings 0 and 1 hold a matrix placing
// the vertex 300 units from the eye; the vertex is drawn as a triangle covering the target. With MESA_KK_DEBUG=msl the driver prints the generated MSL.
//
//   cc -O1 -Ithird_party/vulkan/include tests/vs_probe.c build/kosmickrisp/libvulkan.1.dylib -o build/vs_probe
//   VK_ICD_FILENAMES=build/kosmickrisp/kosmickrisp_icd.json build/vs_probe vs.spv fs.spv STRIDE loc:fmt:off... -- hexbytes
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)
static VkDevice dev; static VkPhysicalDevice pd;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    exit(1);
}
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; } Buf;
static Buf buffer(VkDeviceSize n, VkBufferUsageFlags u) {
    Buf r;
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = u };
    CK(vkCreateBuffer(dev, &ci, NULL, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 6) };
    CK(vkAllocateMemory(dev, &ai, NULL, &r.m)); CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    CK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p)); memset(r.p, 0, n);
    return r;
}
static VkShaderModule module(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n); if (fread(code, 1, (size_t)n, f) != (size_t)n) exit(1); fclose(f);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code };
    VkShaderModule m; CK(vkCreateShaderModule(dev, &ci, NULL, &m)); return m;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: vs_probe vs.spv fs.spv stride loc:fmt:off... -- hexbytes\n"); return 2; }
    uint32_t stride = (uint32_t)atoi(argv[3]);
    VkVertexInputAttributeDescription at[16]; uint32_t na = 0; int i = 4;
    for (; i < argc && strcmp(argv[i], "--"); i++) { unsigned l, f, o; sscanf(argv[i], "%u:%u:%u", &l, &f, &o); at[na++] = (VkVertexInputAttributeDescription){ l, 0, (VkFormat)f, o }; }
    const char *hex = i + 1 < argc ? argv[i + 1] : "";

    const char *iext[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo = &app, .enabledExtensionCount = 1, .ppEnabledExtensionNames = iext };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t npd = 1; vkEnumeratePhysicalDevices(inst, &npd, &pd);
    float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL }, { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL } };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = module(argv[1]), .pName = "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = module(argv[2]), .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = at };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, 1, 1, 0, 1 }; VkRect2D sc = { { 0, 0 }, { 1, 1 } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .layout = pl };
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe));

    // vertex: the given bytes; uniforms: binding 0 = mvp rows (x,y,z,w) such that the point lands at (0,0) with w = 300
    Buf vbuf = buffer(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), u0 = buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT), u1 = buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    // three copies of the vertex, as a triangle covering the 1x1 target (positions at offset 0, R32G32B32_SFLOAT)
    static const float tri[3][3] = { { -1, -1, 0 }, { 3, -1, 0 }, { -1, 3, 0 } };
    for (int v = 0; v < 3; v++) {
        uint8_t *d = (uint8_t *)vbuf.p + v * stride;
        for (size_t k = 0; hex[2 * k] && hex[2 * k + 1]; k++) { unsigned x; sscanf(hex + 2 * k, "%2x", &x); d[k] = (uint8_t)x; }
        memcpy(d, tri[v], 12);
    }
    // clip = (300x, 300y, 150, 300): eye distance 300, depth 0.5
    float *m = u0.p; m[0] = 300; m[5] = 300; m[11] = 150; m[15] = 300; m[16] = 1;
    float *l = u1.p; l[1] = 1; l[2] = 0; l[4] = 1; l[5] = 0;   // fogparms, particlestscalebias
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo bi[2] = { { u0.b, 0, VK_WHOLE_SIZE }, { u1.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
                                  { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] } };
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = cf, .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = 1, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    VkImage img; CK(vkCreateImage(dev, &ic, NULL, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
    VkDeviceMemory im; CK(vkAllocateMemory(dev, &ai, NULL, &im)); CK(vkBindImageMemory(dev, img, im, 0));
    VkImageViewCreateInfo ivc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = cf, .subresourceRange = { 1, 0, 1, 0, 1 } };
    VkImageView iv; CK(vkCreateImageView(dev, &ivc, NULL, &iv));
    Buf out = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
    VkCommandBuffer c; CK(vkAllocateCommandBuffers(dev, &cai, &c));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkBeginCommandBuffer(c, &cbi));
    VkImageMemoryBarrier b0 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = ~0u, .dstQueueFamilyIndex = ~0u, .image = img, .subresourceRange = { 1, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
    VkRenderingAttachmentInfo ra = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = { -7, -7, -7, -7 } };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 1, 1 } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ra };
    vkCmdBeginRendering(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    VkDeviceSize zero = 0; vkCmdBindVertexBuffers(c, 0, 1, &vbuf.b, &zero);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRendering(c);
    b0.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b0.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b0.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
    VkBufferImageCopy rg = { 0, 0, 0, { 1, 0, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
    vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.b, 1, &rg);
    CK(vkEndCommandBuffer(c));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c };
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    float *o = out.p;
    printf("output: %g %g %g %g\n", o[0], o[1], o[2], o[3]);
    return 0;
}
