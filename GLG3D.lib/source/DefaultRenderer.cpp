/**
   \file GLG3D/DefaultRenderer.cpp
 
   \maintainer Morgan McGuire, http://graphics.cs.williams.edu

   \created 2014-12-21
   \edited  2016-09-07

   Copyright 2000-2016, Morgan McGuire.
   All rights reserved.
*/
#include "G3D/fileutils.h"
#include "GLG3D/DefaultRenderer.h"
#include "GLG3D/RenderDevice.h"
#include "GLG3D/Framebuffer.h"
#include "GLG3D/LightingEnvironment.h"
#include "GLG3D/Camera.h"
#include "GLG3D/Surface.h"
#include "GLG3D/AmbientOcclusion.h"
#include "GLG3D/SkyboxSurface.h"
#include "GLG3D/GApp.h"
#include <regex>

namespace G3D {

#define HIGH_PRECISION_OIT_FORMAT ImageFormat::RGBA16F()
#define HIGH_PRECISION_OIT_FORMAT_RG ImageFormat::RG16F()

DefaultRenderer::DefaultRenderer() :
    m_deferredShading(false),
    m_orderIndependentTransparency(false),
    m_oitLowResDownsampleFactor(4), 
    m_oitUpsampleFilterRadius(2), 
    m_oitHighPrecision(true) {

    reloadWriteDeclaration();
}


void DefaultRenderer::reloadWriteDeclaration() {
    // Includes single-line comments and newlines
    const String& originalDeclaration = readWholeFile(System::findDataFile("shader/DefaultRenderer/DefaultRenderer_OIT_writePixel.glsl"));    
    const std::string declarationWithoutSingleLineComments(std::regex_replace(std::string(originalDeclaration.c_str()), std::regex("//.*\n"), "\n"));
    const String declarationWithoutNewlines(std::regex_replace(declarationWithoutSingleLineComments.c_str(), std::regex("[\r\n]"), "").c_str());
    m_oitWriteDeclaration = declarationWithoutNewlines;
}


void DefaultRenderer::allocateAllOITBuffers
   (RenderDevice*                   rd,
    bool                            highPrecision) {
    
    const int lowResWidth  = rd->width()  / m_oitLowResDownsampleFactor;
    const int lowResHeight = rd->height() / m_oitLowResDownsampleFactor;

    m_oitFramebuffer = Framebuffer::create("G3D::DefaultRenderer::m_oitFramebuffer");
    allocateOITFramebufferAttachments(rd, m_oitFramebuffer, rd->width(), rd->height(), highPrecision);
    m_oitLowResFramebuffer = Framebuffer::create("G3D::DefaultRenderer::m_oitLowResFramebuffer");

    allocateOITFramebufferAttachments(rd, m_oitLowResFramebuffer, lowResWidth, lowResHeight, highPrecision);

    const ImageFormat* depthFormat = rd->drawFramebuffer()->texture(Framebuffer::DEPTH)->format();
    shared_ptr<Texture> lowResDepthBuffer = Texture::createEmpty("G3D::DefaultRenderer::lowResDepth", lowResWidth, lowResHeight, depthFormat);
    m_oitLowResFramebuffer->set(Framebuffer::DEPTH, lowResDepthBuffer);

    m_backgroundFramebuffer = Framebuffer::create
        (Texture::createEmpty("G3D::DefaultRenderer::backgroundTexture", 
            rd->width(), rd->height(), rd->drawFramebuffer()->texture(0)->format()));

    m_blurredBackgroundFramebuffer = Framebuffer::create
        (Texture::createEmpty("G3D::DefaultRenderer::backgroundBlurredTexture", 
            rd->width(), rd->height(), rd->drawFramebuffer()->texture(0)->format()));

    m_blurredBackgroundFramebuffer->set(Framebuffer::COLOR1, Texture::createEmpty("G3D::DefaultRenderer::radiusBlurredTexture",
        rd->width(), rd->height(), ImageFormat::R32F()));

    m_csOctLowResNormalFramebuffer = Framebuffer::create(Texture::createEmpty("G3D::DefaultRenderer::m_csOctLowResNormalFramebuffer", 
            lowResWidth, lowResHeight, ImageFormat::RG8_SNORM()));

}


void DefaultRenderer::allocateOITFramebufferAttachments
   (RenderDevice*                   rd, 
    const shared_ptr<Framebuffer>&  oitFramebuffer, 
    int                             w,
    int                             h,
    bool                            highPrecision) {
    
    oitFramebuffer->set(Framebuffer::COLOR0, Texture::createEmpty(oitFramebuffer->name() + "/RT0 (A)", w, h, highPrecision ? HIGH_PRECISION_OIT_FORMAT : ImageFormat::RGBA16F()));
    oitFramebuffer->setClearValue(Framebuffer::COLOR0, Color4::zero());
    {
        const shared_ptr<Texture>& texture = Texture::createEmpty(oitFramebuffer->name() + "/RT1 (Brgb, D)", w, h, highPrecision ? HIGH_PRECISION_OIT_FORMAT : ImageFormat::RGBA8());
        texture->visualization.channels = Texture::Visualization::RGB;
        oitFramebuffer->set(Framebuffer::COLOR1, texture);
        oitFramebuffer->setClearValue(Framebuffer::COLOR1, Color4(1, 1, 1, 0));
    }
    {
        const shared_ptr<Texture>& texture = Texture::createEmpty(oitFramebuffer->name() + "/RT2 (delta)", w, h, highPrecision ? HIGH_PRECISION_OIT_FORMAT_RG : ImageFormat::RG8_SNORM());
        oitFramebuffer->set(Framebuffer::COLOR2, texture);
        oitFramebuffer->setClearValue(Framebuffer::COLOR2, Color4::zero());
    }
}


void DefaultRenderer::clearAndRenderToOITFramebuffer
   (RenderDevice*                   rd,
    const shared_ptr<Framebuffer>&  oitFramebuffer,
    Array < shared_ptr<Surface>>&   surfaceArray,
    const shared_ptr<GBuffer>&      gbuffer,
    const LightingEnvironment&      environment) {

    rd->setFramebuffer(oitFramebuffer);
    rd->clearFramebuffer(true, false);

    // Allow writePixel to read the depth buffer. Make the name unique so that it doesn't conflict with the depth texture
    // passed to ParticleSurface for soft particle depth testing
    oitFramebuffer->texture(Framebuffer::DEPTH)->setShaderArgs(oitFramebuffer->uniformTable, "_depthTexture.", Sampler::buffer());
    oitFramebuffer->uniformTable.setUniform("_clipInfo", gbuffer->camera()->projection().reconstructFromDepthClipInfo());
    oitFramebuffer->uniformTable.setMacro("DECLARE_writePixel", m_oitWriteDeclaration);

    rd->pushState(oitFramebuffer); {
        // Set blending modes
        // Accum (A)
        rd->setBlendFunc(RenderDevice::BLEND_ONE,  RenderDevice::BLEND_ONE,                 RenderDevice::BLENDEQ_ADD, RenderDevice::BLENDEQ_SAME_AS_RGB, Framebuffer::COLOR0);

        // Background modulation (beta) and diffusion (D)
        rd->setBlendFunc(Framebuffer::COLOR1,
                         RenderDevice::BLEND_ZERO, RenderDevice::BLEND_ONE_MINUS_SRC_COLOR, RenderDevice::BLENDEQ_ADD, 
                         RenderDevice::BLEND_ONE,  RenderDevice::BLEND_ONE,                 RenderDevice::BLENDEQ_ADD);

        // Refraction (delta)
        rd->setBlendFunc(RenderDevice::BLEND_ONE,  RenderDevice::BLEND_ONE,                 RenderDevice::BLENDEQ_ADD, RenderDevice::BLENDEQ_SAME_AS_RGB, Framebuffer::COLOR2);

        forwardShade(rd, surfaceArray, gbuffer, environment, RenderPassType::SINGLE_PASS_UNORDERED_BLENDED_SAMPLES, m_oitWriteDeclaration, ARBITRARY);
    } rd->popState();
}


void DefaultRenderer::resizeOITBuffersIfNeeded
   (const int                       width, 
    const int                       height, 
    const int                       lowResWidth,
    const int                       lowResHeight) {
    
    if ((m_oitFramebuffer->width() != width) ||
        (m_oitFramebuffer->height() != height) ||
        (m_oitLowResFramebuffer->width() != lowResWidth) ||
        (m_oitLowResFramebuffer->height() != lowResHeight)) {

        m_oitFramebuffer->resize(width, height);
        m_oitLowResFramebuffer->resize(lowResWidth, lowResHeight);
        m_csOctLowResNormalFramebuffer->resize(lowResWidth, lowResHeight);
        m_backgroundFramebuffer->resize(width, height);
        m_blurredBackgroundFramebuffer->resize(width, height);
    }
}


void DefaultRenderer::computeLowResDepthAndNormals(RenderDevice* rd, const shared_ptr<Texture>& csHighResNormalTexture) {
    // Nearest-neighbor downsample depth
    Texture::copy
       (m_oitFramebuffer->texture(Framebuffer::DEPTH),
        m_oitLowResFramebuffer->texture(Framebuffer::DEPTH),
        0, 0,
        float(m_oitLowResDownsampleFactor),
        Vector2int16(0, 0),
        CubeFace::POS_X,
        CubeFace::POS_X,
        rd,
        false);

    // Downsample and convert normals to Octahedral format
    if (notNull(csHighResNormalTexture)) {
        rd->push2D(m_csOctLowResNormalFramebuffer); {
            Args args;

            csHighResNormalTexture->setShaderArgs(args, "csHighResNormalTexture.", Sampler::buffer());
            args.setUniform("lowResDownsampleFactor", m_oitLowResDownsampleFactor);
            args.setRect(rd->viewport());
            LAUNCH_SHADER("DefaultRenderer_downsampleNormal.pix", args);
        } rd->pop2D();
    }
}


void DefaultRenderer::render
   (RenderDevice*                       rd, 
    const shared_ptr<Framebuffer>&      framebuffer,
    const shared_ptr<Framebuffer>&      depthPeelFramebuffer,
    LightingEnvironment&                lightingEnvironment,
    const shared_ptr<GBuffer>&          gbuffer, 
    const Array<shared_ptr<Surface>>&   allSurfaces) {

    alwaysAssertM(! lightingEnvironment.ambientOcclusionSettings.enabled || notNull(lightingEnvironment.ambientOcclusion),
        "Ambient occlusion is enabled but no ambient occlusion object is bound to the lighting environment");

    const shared_ptr<Camera>& camera = gbuffer->camera();

    // Share the depth buffer with the forward-rendering pipeline
    framebuffer->set(Framebuffer::DEPTH, gbuffer->texture(GBuffer::Field::DEPTH_AND_STENCIL));
    if (notNull(depthPeelFramebuffer)) {
        depthPeelFramebuffer->resize(framebuffer->width(), framebuffer->height());
    }

    // Cull and sort
    Array<shared_ptr<Surface> > sortedVisibleSurfaces, forwardOpaqueSurfaces, forwardBlendedSurfaces;
    cullAndSort(gbuffer, allSurfaces, sortedVisibleSurfaces, forwardOpaqueSurfaces, forwardBlendedSurfaces);

    debugAssert(framebuffer);
    // Bind the main framebuffer
    rd->pushState(framebuffer); {
        rd->clear();
        rd->setProjectionAndCameraMatrix(camera->projection(), camera->frame());
        
        const bool needDepthPeel = lightingEnvironment.ambientOcclusionSettings.useDepthPeelBuffer && lightingEnvironment.ambientOcclusionSettings.enabled;
        computeGBuffer(rd, sortedVisibleSurfaces, gbuffer, needDepthPeel ? depthPeelFramebuffer : nullptr, lightingEnvironment.ambientOcclusionSettings.depthPeelSeparationHint);

        // Shadowing + AO
        computeShadowing(rd, allSurfaces, gbuffer, depthPeelFramebuffer, lightingEnvironment);

        // Maybe launch deferred pass
        if (deferredShading()) {
            renderDeferredShading(rd, gbuffer, lightingEnvironment);
        }

        // Main forward pass
        renderOpaqueSamples(rd, deferredShading() ? forwardOpaqueSurfaces : sortedVisibleSurfaces, gbuffer, lightingEnvironment);

        // Prepare screen-space lighting for the *next* frame
        lightingEnvironment.copyScreenSpaceBuffers(framebuffer, gbuffer->colorGuardBandThickness(), gbuffer->depthGuardBandThickness());

        renderOpaqueScreenSpaceRefractingSamples(rd, deferredShading() ? forwardOpaqueSurfaces : sortedVisibleSurfaces, gbuffer, lightingEnvironment);

        // Samples that require blending
        if (m_orderIndependentTransparency) {
            renderOrderIndependentBlendedSamples(rd, forwardBlendedSurfaces, gbuffer, lightingEnvironment);
        } else {
            renderSortedBlendedSamples(rd, forwardBlendedSurfaces, gbuffer, lightingEnvironment);
        }
                
    } rd->popState();
}


void DefaultRenderer::renderDeferredShading(RenderDevice* rd, const shared_ptr<GBuffer>& gbuffer, const LightingEnvironment& environment) {
    // Make a pass over the screen, performing shading
    rd->push2D(); {
        rd->setGuardBandClip2D(gbuffer->colorGuardBandThickness());

        // Don't shade the skybox on this pass because it will be forward rendered
        rd->setDepthTest(RenderDevice::DEPTH_GREATER);
        Args args;

        environment.setShaderArgs(args);
        gbuffer->setShaderArgsRead(args, "gbuffer_");

        args.setRect(rd->viewport());

        LAUNCH_SHADER("DefaultRenderer_deferredShade.pix", args);
    } rd->pop2D();
}


void DefaultRenderer::renderOpaqueSamples
   (RenderDevice*                       rd, 
    Array<shared_ptr<Surface> >&        surfaceArray, 
    const shared_ptr<GBuffer>&          gbuffer, 
    const LightingEnvironment&          environment) {

    BEGIN_PROFILER_EVENT("DefaultRenderer::renderOpaqueSamples");
    forwardShade(rd, surfaceArray, gbuffer, environment, RenderPassType::OPAQUE_SAMPLES, Surface::defaultWritePixelDeclaration(), ARBITRARY);
    END_PROFILER_EVENT();
}


void DefaultRenderer::renderOpaqueScreenSpaceRefractingSamples
   (RenderDevice*                       rd, 
    Array<shared_ptr<Surface> >&        surfaceArray, 
    const shared_ptr<GBuffer>&          gbuffer, 
    const LightingEnvironment&          environment) {

    BEGIN_PROFILER_EVENT("DefaultRenderer::renderOpaqueScreenSpaceRefractingSamples");
    forwardShade(rd, surfaceArray, gbuffer, environment, RenderPassType::UNBLENDED_SCREEN_SPACE_REFRACTION_SAMPLES, Surface::defaultWritePixelDeclaration(), ARBITRARY);
    END_PROFILER_EVENT();
}


void DefaultRenderer::renderSortedBlendedSamples       
   (RenderDevice*                       rd, 
    Array<shared_ptr<Surface> >&        surfaceArray, 
    const shared_ptr<GBuffer>&          gbuffer, 
    const LightingEnvironment&          environment) {

    BEGIN_PROFILER_EVENT("DefaultRenderer::renderSortedBlendedSamples");
    forwardShade(rd, surfaceArray, gbuffer, environment, RenderPassType::MULTIPASS_BLENDED_SAMPLES, Surface::defaultWritePixelDeclaration(), BACK_TO_FRONT);
    END_PROFILER_EVENT();
}


void DefaultRenderer::renderOrderIndependentBlendedSamples
   (RenderDevice*                       rd, 
    Array<shared_ptr<Surface> >&        surfaceArray, 
    const shared_ptr<GBuffer>&          gbuffer, 
    const LightingEnvironment&          environment) {

    BEGIN_PROFILER_EVENT("DefaultRenderer::renderOrderIndependentBlendedSamples");

    if (surfaceArray.size() > 0) {

        // Categorize the surfaces by desired resolution
        static Array< shared_ptr<Surface> > hiResSurfaces;
        static Array< shared_ptr<Surface> > loResSurfaces;

        for (const shared_ptr<Surface>& s : surfaceArray) {
            if (s->preferLowResolutionTransparency()) {
                loResSurfaces.append(s);
            } else {
                hiResSurfaces.append(s);
            }
        }

        const int lowResWidth  = rd->width()  / m_oitLowResDownsampleFactor;
        const int lowResHeight = rd->height() / m_oitLowResDownsampleFactor;

        // Test whether we need to allocate the OIT buffers 
        // (i.e., are they non-existent or at the wrong precision)
        if (isNull(m_oitFramebuffer) || 
            ((m_oitFramebuffer->texture(0)->format() == HIGH_PRECISION_OIT_FORMAT) != m_oitHighPrecision)) {
            allocateAllOITBuffers(rd, m_oitHighPrecision);
        }

        resizeOITBuffersIfNeeded(rd->width(), rd->height(), lowResWidth, lowResHeight);

        // Re-use the depth from the main framebuffer (for depth testing only)
        m_oitFramebuffer->set(Framebuffer::DEPTH, rd->drawFramebuffer()->texture(Framebuffer::DEPTH));
        
        // Copy the current color buffer to the background buffer, since we'll be compositing into
        // the color buffer at the end of the OIT process
        rd->drawFramebuffer()->blitTo(rd, m_backgroundFramebuffer, false, false, false, false, true);
        
        ////////////////////////////////////////////////////////////////////////////////////
        //
        // Accumulation pass over (3D) transparent surfaces
        //        
        const shared_ptr<Framebuffer> oldBuffer = rd->drawFramebuffer();

        clearAndRenderToOITFramebuffer(rd, m_oitFramebuffer, hiResSurfaces, gbuffer, environment);

        if (loResSurfaces.size() > 0) {
            // Create a low-res copy of the depth (and normal) buffers for depth testing and then
            // for use as the key for bilateral upsampling.
            computeLowResDepthAndNormals(rd, gbuffer->texture(GBuffer::Field::CS_NORMAL));

            clearAndRenderToOITFramebuffer(rd, m_oitLowResFramebuffer, loResSurfaces, gbuffer, environment);
            rd->push2D(m_oitFramebuffer); {
                // Set blending modes
                // Accum (A)
                rd->setBlendFunc(RenderDevice::BLEND_ONE, RenderDevice::BLEND_ONE, RenderDevice::BLENDEQ_ADD, RenderDevice::BLENDEQ_SAME_AS_RGB, Framebuffer::COLOR0);

                // Background modulation (beta) and diffusion (D)
                rd->setBlendFunc(Framebuffer::COLOR1,
                    RenderDevice::BLEND_ZERO, RenderDevice::BLEND_ONE_MINUS_SRC_COLOR, RenderDevice::BLENDEQ_ADD,
                    RenderDevice::BLEND_ONE, RenderDevice::BLEND_ONE, RenderDevice::BLENDEQ_ADD);

                // Delta (refraction)
                rd->setBlendFunc(RenderDevice::BLEND_ONE, RenderDevice::BLEND_ONE, RenderDevice::BLENDEQ_ADD, RenderDevice::BLENDEQ_SAME_AS_RGB, Framebuffer::COLOR2);

                Args args;
                args.setMacro("FILTER_RADIUS",                              m_oitUpsampleFilterRadius);

                args.setUniform("sourceDepth",                              m_oitLowResFramebuffer->texture(Framebuffer::DEPTH), Sampler::buffer());
                args.setUniform("destDepth",                                m_oitFramebuffer->texture(Framebuffer::DEPTH), Sampler::buffer());
                args.setUniform("sourceSize",                               Vector2(float(m_oitLowResFramebuffer->width()), float(m_oitLowResFramebuffer->height())));
                args.setUniform("accumTexture",                             m_oitLowResFramebuffer->texture(0), Sampler::buffer());
                args.setUniform("backgroundModulationAndDiffusionTexture",  m_oitLowResFramebuffer->texture(1), Sampler::buffer());
                args.setUniform("deltaTexture",                             m_oitLowResFramebuffer->texture(2), Sampler::buffer());
                args.setUniform("downsampleFactor",                         m_oitLowResDownsampleFactor);

                const shared_ptr<Texture>& destNormal = gbuffer->texture(GBuffer::Field::CS_NORMAL);
                if (notNull(destNormal)) {
                    args.setMacro("HAS_NORMALS", true);
                    destNormal->setShaderArgs(args, "destNormal.", Sampler::buffer()); 
                    args.setUniform("sourceOctNormal", m_csOctLowResNormalFramebuffer->texture(0), Sampler::buffer());
                } else {
                    args.setMacro("HAS_NORMALS", false);
                }

                args.setRect(rd->viewport());
                LAUNCH_SHADER("DefaultRenderer_upsampleOIT.pix", args);
            } rd->pop2D();
        }

        // Remove the color buffer binding which is shared with the main framebuffer, so that we don't 
        // clear it on the next pass through this function. Not done for colored OIT
        // m_oitFramebuffer->set(Framebuffer::COLOR2, shared_ptr<Texture>());
        rd->setFramebuffer(oldBuffer);

        ////////////////////////////////////////////////////////////////////////////////////
        //
        // 2D compositing pass
        //

        rd->push2D(); {
            rd->setDepthTest(RenderDevice::DEPTH_ALWAYS_PASS);
            Args args;          
            m_backgroundFramebuffer->texture(0)->setShaderArgs(args, "backgroundTexture.", Sampler(WrapMode::CLAMP, InterpolateMode::BILINEAR_NO_MIPMAP));

            const Projection& projection = gbuffer->camera()->projection();
            const float ppd = 0.05f * rd->viewport().height() / tan(projection.fieldOfViewAngles(rd->viewport()).y);
            args.setUniform("pixelsPerDiffusion2", square(ppd));
            
            m_oitFramebuffer->texture(0)->setShaderArgs(args, "accumTexture.", Sampler::buffer());
            m_oitFramebuffer->texture(1)->setShaderArgs(args, "backgroundModulationAndDiffusionTexture.", Sampler::buffer());
            m_oitFramebuffer->texture(2)->setShaderArgs(args, "deltaTexture.", Sampler::buffer());
            args.setRect(rd->viewport());
            LAUNCH_SHADER("DefaultRenderer_compositeWeightedBlendedOIT.pix", args);
        } rd->pop2D();

        hiResSurfaces.fastClear();
        loResSurfaces.fastClear();
    }

    END_PROFILER_EVENT();
}


} //namespace
