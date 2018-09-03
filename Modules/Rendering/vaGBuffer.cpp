///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2016, Intel Corporation
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated 
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of 
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
// SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Author(s):  Filip Strugar (filip.strugar@intel.com)
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "vaGBuffer.h"

#include "Rendering/vaRenderDeviceContext.h"

using namespace VertexAsylum;

vaGBuffer::vaGBuffer( const vaRenderingModuleParams & params ) : 
    vaRenderingModule( params ),
    m_pixelShader( params.RenderDevice ),
    m_depthToViewspaceLinearPS( params.RenderDevice ),
    m_debugDrawDepthPS( params.RenderDevice ),
    m_debugDrawDepthViewspaceLinearPS( params.RenderDevice ),
    m_debugDrawNormalMapPS( params.RenderDevice ),
    m_debugDrawAlbedoPS( params.RenderDevice ),
    m_debugDrawRadiancePS( params.RenderDevice )
{
    m_debugInfo             = "GBuffer (uninitialized - forgot to call RenderTick?)";
    m_debugSelectedTexture  = -1;

    m_resolution            = vaVector2i( 0, 0 );
    m_sampleCount           = 1;
    m_deferredEnabled       = false;

    m_shadersDirty          = true;
    m_shaderFileToUse       = L"vaGBuffer.hlsl";
}

vaGBuffer::~vaGBuffer( )
{

}

void vaGBuffer::IHO_Draw( )
{
#ifdef VA_IMGUI_INTEGRATION_ENABLED
    struct TextureInfo
    {
        string                  Name;
        shared_ptr<vaTexture>   Texture;
    };

    std::vector< TextureInfo > textures;

    textures.push_back( { "Depth Buffer", m_depthBuffer } );
    textures.push_back( { "Depth Buffer Viewspace Linear", m_depthBufferViewspaceLinear } );
    textures.push_back( { "Normal Map", m_normalMap } );
    textures.push_back( { "Albedo", m_albedo } );
    textures.push_back( { "Radiance", m_radiance} );
    textures.push_back( { "OutputColor", m_outputColorView } );

    for( size_t i = 0; i < textures.size(); i++ )
    {
        if( ImGui::Selectable( textures[i].Name.c_str(), m_debugSelectedTexture == i ) )
        {
            if( m_debugSelectedTexture == i )
                m_debugSelectedTexture = -1;
            else
                m_debugSelectedTexture = (int)i;
        }
    }
#endif
}

bool vaGBuffer::ReCreateIfNeeded( shared_ptr<vaTexture> & inoutTex, int width, int height, vaResourceFormat format, bool needsUAV, int msaaSampleCount, float & inoutTotalSizeSum )
{
//    if( format == vaResourceFormat::Unknown )
//    {
//        inoutTex = nullptr;
//        return true;
//    }

    vaResourceBindSupportFlags bindFlags = vaResourceBindSupportFlags::RenderTarget | vaResourceBindSupportFlags::ShaderResource;
    if( needsUAV )
        bindFlags |= vaResourceBindSupportFlags::UnorderedAccess;

    if( (width == 0) || (height == 0) || (format == vaResourceFormat::Unknown ) )
    {
        inoutTex = nullptr;
        return false;
    }
    else
    {
        vaResourceFormat resourceFormat  = format;
        vaResourceFormat srvFormat       = format;
        vaResourceFormat rtvFormat       = format;
        vaResourceFormat dsvFormat       = vaResourceFormat::Unknown;
        vaResourceFormat uavFormat       = format;

        // handle special cases
        if( format == vaResourceFormat::D32_FLOAT )
        {
            bindFlags = (bindFlags & ~(vaResourceBindSupportFlags::RenderTarget | vaResourceBindSupportFlags::UnorderedAccess)) | vaResourceBindSupportFlags::DepthStencil;
            resourceFormat  = vaResourceFormat::R32_TYPELESS;
            srvFormat       = vaResourceFormat::R32_FLOAT;
            rtvFormat       = vaResourceFormat::Unknown;
            dsvFormat       = vaResourceFormat::D32_FLOAT;
            uavFormat       = vaResourceFormat::Unknown;
        }
        else if( format == vaResourceFormat::D24_UNORM_S8_UINT )
        {
            bindFlags = ( bindFlags & ~( vaResourceBindSupportFlags::RenderTarget | vaResourceBindSupportFlags::UnorderedAccess ) ) | vaResourceBindSupportFlags::DepthStencil;
            resourceFormat = vaResourceFormat::R24G8_TYPELESS;
            srvFormat = vaResourceFormat::R24_UNORM_X8_TYPELESS;
            rtvFormat = vaResourceFormat::Unknown;
            dsvFormat = vaResourceFormat::D24_UNORM_S8_UINT;
            uavFormat = vaResourceFormat::Unknown;
        }
        else if( format == vaResourceFormat::R8G8B8A8_UNORM_SRGB )
        {
            //resourceFormat  = vaResourceFormat::R8G8B8A8_TYPELESS;
            //srvFormat       = vaResourceFormat::R8G8B8A8_UNORM_SRGB;
            //rtvFormat       = vaResourceFormat::R8G8B8A8_UNORM_SRGB;
            //uavFormat       = vaResourceFormat::R8G8B8A8_UNORM;
            uavFormat  = vaResourceFormat::Unknown;
            bindFlags &= ~vaResourceBindSupportFlags::UnorderedAccess;
        }
        else if ( vaResourceFormatHelpers::IsTypeless(format) )
        {
            srvFormat  = vaResourceFormat::Unknown;
            rtvFormat  = vaResourceFormat::Unknown;
            uavFormat  = vaResourceFormat::Unknown;
            //bindFlags &= ~(vaResourceBindSupportFlags::ShaderResource|vaResourceBindSupportFlags::RenderTarget|vaResourceBindSupportFlags::UnorderedAccess);
        }

        if( !needsUAV ) uavFormat       = vaResourceFormat::Unknown;

        if( (inoutTex != nullptr) && (inoutTex->GetSizeX() == width) && (inoutTex->GetSizeY()==height) &&
            (inoutTex->GetResourceFormat()==resourceFormat) && (inoutTex->GetSRVFormat()==srvFormat) && (inoutTex->GetRTVFormat()==rtvFormat) &&
            (inoutTex->GetDSVFormat()==dsvFormat) && (inoutTex->GetUAVFormat()==uavFormat) && (inoutTex->GetSampleCount()==msaaSampleCount) )
            return false;

        inoutTex = vaTexture::Create2D( GetRenderDevice(), resourceFormat, width, height, 1, 1, msaaSampleCount, bindFlags, vaTextureAccessFlags::None, srvFormat, rtvFormat, dsvFormat, uavFormat );
    }
    inoutTotalSizeSum += width * height * vaResourceFormatHelpers::GetPixelSizeInBytes( format ) * msaaSampleCount;
    return true;
}

void vaGBuffer::UpdateResources( int width, int height, int msaaSampleCount, const BufferFormats & formats, bool enableDeferred )
{
    assert( width > 0 );
    assert( height > 0 );

    m_resolution = vaVector2i( width, height );
    m_formats = formats;
    m_sampleCount = msaaSampleCount;
    m_deferredEnabled = enableDeferred;

    float totalSizeInMB = 0.0f;

    ReCreateIfNeeded( m_depthBuffer                 , width, height, m_formats.DepthBuffer,                   false, msaaSampleCount,    totalSizeInMB );    
    ReCreateIfNeeded( m_depthBufferViewspaceLinear  , width, height, m_formats.DepthBufferViewspaceLinear,    false, msaaSampleCount,    totalSizeInMB );
    ReCreateIfNeeded( m_radiance                    , width, height, m_formats.Radiance,                      false, msaaSampleCount,    totalSizeInMB );
    if( ReCreateIfNeeded( m_outputColorTypeless     , width, height, m_formats.OutputColorTypeless,           true, 1,                  totalSizeInMB ) )                  // output is not MSAA, but has UAV
    {
        m_outputColorIgnoreSRGBConvView = vaTexture::CreateView( *m_outputColorTypeless, vaResourceBindSupportFlags::RenderTarget | vaResourceBindSupportFlags::UnorderedAccess | vaResourceBindSupportFlags::ShaderResource, 
            m_formats.OutputColorIgnoreSRGBConvView, m_formats.OutputColorIgnoreSRGBConvView, vaResourceFormat::Unknown, m_formats.OutputColorIgnoreSRGBConvView );
        m_outputColorView = vaTexture::CreateView( *m_outputColorTypeless, vaResourceBindSupportFlags::RenderTarget | vaResourceBindSupportFlags::ShaderResource, 
            m_formats.OutputColorView, m_formats.OutputColorView, vaResourceFormat::Unknown, vaResourceFormat::Unknown );
        if( m_formats.OutputColorR32UINT_UAV == vaResourceFormat::R32_UINT )
            m_outputColorR32UINT_UAV = vaTexture::CreateView( *m_outputColorTypeless, vaResourceBindSupportFlags::UnorderedAccess, vaResourceFormat::Unknown, vaResourceFormat::Unknown, vaResourceFormat::Unknown, m_formats.OutputColorR32UINT_UAV );
        else
            m_outputColorR32UINT_UAV = nullptr;
    }

    if( m_deferredEnabled )
    {
        ReCreateIfNeeded( m_normalMap                   , width, height, m_formats.NormalMap,                     false, msaaSampleCount,    totalSizeInMB );
        ReCreateIfNeeded( m_albedo                      , width, height, m_formats.Albedo,                        false, msaaSampleCount,    totalSizeInMB );
    }
    else
    {
        m_normalMap = nullptr;
        m_albedo    = nullptr;
    }

    totalSizeInMB /= 1024 * 1024;

    m_debugInfo = vaStringTools::Format( "GBuffer (approx. %.2fMB) ", totalSizeInMB );
}

void vaGBuffer::UpdateShaders( )
{
    if( m_shadersDirty )
    {
        m_shadersDirty = false;
    
        m_depthToViewspaceLinearPS->CreateShaderFromFile(        m_shaderFileToUse, "ps_5_0", "DepthToViewspaceLinearPS",         m_staticShaderMacros );
        m_debugDrawDepthPS->CreateShaderFromFile(                m_shaderFileToUse, "ps_5_0", "DebugDrawDepthPS",                 m_staticShaderMacros );
        m_debugDrawDepthViewspaceLinearPS->CreateShaderFromFile( m_shaderFileToUse, "ps_5_0", "DebugDrawDepthViewspaceLinearPS",  m_staticShaderMacros );
        m_debugDrawNormalMapPS->CreateShaderFromFile(            m_shaderFileToUse, "ps_5_0", "DebugDrawNormalMapPS",             m_staticShaderMacros );
        m_debugDrawAlbedoPS->CreateShaderFromFile(               m_shaderFileToUse, "ps_5_0", "DebugDrawAlbedoPS",                m_staticShaderMacros );
        m_debugDrawRadiancePS->CreateShaderFromFile(             m_shaderFileToUse, "ps_5_0", "DebugDrawRadiancePS",              m_staticShaderMacros );
    }
}