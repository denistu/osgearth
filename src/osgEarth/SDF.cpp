/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "SDF"
#include "Math"
#include "Metrics"
#include "FeatureSource"
#include "FeatureRasterizer"
#include "Session"

using namespace osgEarth;
using namespace osgEarth::Util;

namespace
{
    inline bool isPositivePowerOfTwo(unsigned x) {
        return (x & (x - 1)) == 0;
    }

    // https://www.comp.nus.edu.sg/~tants/jfa/i3d06.pdf
    const char* jfa_cs = R"(
    #version 430
    layout(local_size_x=1, local_size_y=1, local_size_z=1) in;

    // output image binding
    layout(binding=0, rg16f) uniform image2D buf;

    uniform int L;

    #define NODATA 32767.0

    float squared_distance_2d(in vec4 a, in vec4 b)
    {
        vec2 c = b.xy-a.xy;
        return dot(c, c);
    }

    void main()
    {
        vec2 pixel_uv = vec2(
            float(gl_WorkGroupID.x) / float(gl_NumWorkGroups.x-1),
            float(gl_WorkGroupID.y) / float(gl_NumWorkGroups.y-1));

        int s = int(gl_WorkGroupID.x);
        int t = int(gl_WorkGroupID.y);

        vec4 pixel_points_to = imageLoad(buf, ivec2(gl_WorkGroupID));
        if (pixel_points_to.x == NODATA)
            return;

        vec4 remote;
        vec4 remote_points_to;

        for(int rs = s - L; rs <= s + L; rs += L)
        {
            if (rs < 0 || rs >= gl_NumWorkGroups.x)
                continue;

            remote.x = float(rs);

            for(int rt = t - L; rt <= t + L; rt += L)
            {
                if (rt < 0 || rt >= gl_NumWorkGroups.y)
                    continue;

                if (rs == s && rt == t)
                    continue;

                remote.y = float(rt);

                remote_points_to = imageLoad(buf, ivec2(rs,rt));
                if (remote_points_to.x == NODATA)
                {
                    imageStore(buf, ivec2(rs,rt), pixel_points_to);
                }
                else
                {
                    // compare the distances and pick the closest.
                    float d_existing = squared_distance_2d(remote, remote_points_to);
                    float d_possible = squared_distance_2d(remote, pixel_points_to);

                    if (d_possible < d_existing)
                    {
                        imageStore(buf, ivec2(rs,rt), pixel_points_to);
                    }
                }
            }
        }
    }
    )";
}

SDFGenerator::SDFGenerator() :
    _useGPU(false)
{
    //nop
}

void
SDFGenerator::setUseGPU(bool value)
{
    _useGPU = value;

    if (_useGPU && !_program.valid())
    {
        _program = new osg::Program();
        _program->addShader(new osg::Shader(osg::Shader::COMPUTE, jfa_cs));
    }
}

GeoImage
SDFGenerator::allocateSDF(
    unsigned size,
    const GeoExtent& extent) const
{
    osg::ref_ptr<osg::Image> sdf = new osg::Image();
    sdf->allocateImage(size, size, 1, GL_RED, GL_UNSIGNED_BYTE);
    sdf->setInternalTextureFormat(GL_R8);
    ImageUtils::PixelWriter write(sdf.get());
    write.assign(Color(1, 1, 1, 1));
    return GeoImage(sdf.release(), extent);
}

bool
SDFGenerator::createNearestNeighborField(
    const FeatureList& features,
    Session* session,
    unsigned nnfieldSize,
    const GeoExtent& extent,
    GeoImage& nnfield,
    Cancelable* progress) const
{
    if (features.empty())
        return false;

    OE_SOFT_ASSERT_AND_RETURN(extent.isValid(), __func__, false);
    OE_SOFT_ASSERT_AND_RETURN(isPositivePowerOfTwo(nnfieldSize), __func__, false);

    // Render features to a temporary image
    Style style;
    if (features.front()->getGeometry()->isLinear())
        style.getOrCreate<LineSymbol>()->stroke()->color() = Color::Black;
    else
        style.getOrCreate<PolygonSymbol>()->fill()->color() = Color::Black;

    osg::ref_ptr<const FeatureProfile> fp;
    if (session && session->getFeatureSource())
        fp = session->getFeatureSource()->getFeatureProfile();
    else
        fp = new FeatureProfile(extent);

    FeatureRasterizer rasterizer(nnfieldSize, nnfieldSize, extent, Color(1, 1, 1, 0));
    rasterizer.render(session, style, fp.get(), features);
    GeoImage source = rasterizer.finalize();

    return createNearestNeighborField(
        source,
        nnfield,
        progress);
}

bool
SDFGenerator::createNearestNeighborField(
    const GeoImage& inputRaster,
    GeoImage& nnfield,
    Cancelable* progress) const
{
    // Convert pixels to local coordinates relative to the lower-left corner of extent
    if (!nnfield.valid())
    {
        osg::Image* image = new osg::Image();
        image->allocateImage(inputRaster.getImage()->s(), inputRaster.getImage()->t(), 1, GL_RG, GL_FLOAT);
        image->setInternalTextureFormat(GL_RG16F);
        nnfield = std::move(GeoImage(image, inputRaster.getExtent()));
    }

    // actually need to write to the GeoImage, and that's OK.
    osg::Image* nnimage = const_cast<osg::Image*>(nnfield.getImage());

    ImageUtils::PixelReader read_raster(inputRaster.getImage());
    ImageUtils::PixelWriter write_nnf(nnimage);

    constexpr float NODATA = 32767.0f;
    osg::Vec4f nodata(NODATA, NODATA, NODATA, NODATA);
    osg::Vec4f pixel, coord;
    //ImageUtils::ImageIterator iter(inputRaster.getImage());
    GeoImageIterator iter(inputRaster);
    iter.forEachPixel([&]()
        {
            read_raster(pixel, iter.s(), iter.t());
            if (pixel.a() > 0.0f)
                coord.set((float)iter.s(), (float)iter.t(), 0.0f, 0.0f);
                //coord.set(iter.u(), iter.v(), 0, 0);
            else
                coord = nodata;

            write_nnf(coord, iter.s(), iter.t());
        }
    );

    if (_useGPU && GPUJobArena::arena().getGraphicsContext().valid())
    {
        compute_nnf_on_gpu(nnimage);
    }
    else
    {
        compute_nnf_on_cpu(nnimage);
    }

    return true;
}

void
SDFGenerator::createDistanceField(
    const GeoImage& nnfield,
    GeoImage& sdf,
    float span,
    float lo,
    float hi,
    Cancelable* progress) const
{
    OE_SOFT_ASSERT_AND_RETURN(nnfield.valid(), __func__, );
    OE_SOFT_ASSERT_AND_RETURN(sdf.valid(), __func__, );

    // That's OK.
    osg::Image* sdfimage = const_cast<osg::Image*>(sdf.getImage());

    ImageUtils::PixelReader read_sdf(sdfimage);
    ImageUtils::PixelWriter write_sdf(sdfimage);

    ImageUtils::PixelReader read_nnf(nnfield.getImage());
    read_nnf.setBilinear(false);
    
    GeoImageIterator i(sdf);

    osg::Vec4f me, closest, pixel;
    int c = 0; // clamp((int)(channel - GL_RED), 0, 3);

#if 0

    int bias_s = (nnfield.getImage()->s() - sdf.getImage()->s()) / 2;
    int bias_t = (nnfield.getImage()->t() - sdf.getImage()->t()) / 2;

    int nnf_s, nnf_t;

    float cellSize = 1.0f / (float)(nnfield.getImage()->s() - 1);

    constexpr float NODATA = 32767.0f;

    i.forEachPixel([&]()
        {
            read_sdf(pixel, i.s(), i.t());

            // convert ndc coords to the NNF domain
            nnf_s = i.s() + bias_s; 
            nnf_t = i.t() + bias_t;

            OE_SOFT_ASSERT(nnf_s >= 0 && nnf_s < nnfield.getImage()->s(), __func__);
            OE_SOFT_ASSERT(nnf_t >= 0 && nnf_t < nnfield.getImage()->t(), __func__);

            me.set((float)nnf_s, (float)nnf_t, 0, 0);
            read_nnf(closest, nnf_s, nnf_t);

            OE_SOFT_ASSERT(closest.x() != NODATA, __func__);

            float d = distance2D(me, closest);

            d = unitremap(span * (d * cellSize), lo, hi);
            //d = unitremap(d * span, lo, hi);
            if (d < pixel[c])
            {
                pixel[c] = d;
                write_sdf(pixel, i.s(), i.t());
            }
        }
    );
#endif

#if 1
    osg::Vec2f bias(
        (sdf.getExtent().xMin() - nnfield.getExtent().xMin()) / nnfield.getExtent().width(),
        (sdf.getExtent().yMin() - nnfield.getExtent().yMin()) / nnfield.getExtent().height());

    osg::Vec2f scale(
        sdf.getExtent().width() / nnfield.getExtent().width(),
        sdf.getExtent().height() / nnfield.getExtent().height());

    float nnf_u, nnf_v;

    float cellSize = 1.0f / (float)(nnfield.getImage()->s() - 1);

    i.forEachPixelOnCenter([&]()
        {
            read_sdf(pixel, i.s(), i.t());

            // convert ndc coords to the NNF domain
            nnf_u = clamp(i.u() * scale.x() + bias.x(), 0.0, 1.0);
            nnf_v = clamp(i.v() * scale.y() + bias.y(), 0.0, 1.0);
            me.set(floor(nnf_u * nnfield.getImage()->s()), floor(nnf_v*nnfield.getImage()->t()), 0, 0);
            read_nnf(closest, nnf_u, nnf_v);

            float d = distance2D(me, closest);
            d = unitremap(d * cellSize * span, lo, hi);
            if (d < pixel[c])
            {
                pixel[c] = d;
                write_sdf(pixel, i.s(), i.t());
            }
        });
#endif
}


void
SDFGenerator::compute_nnf_on_gpu(osg::Image* image) const
{
    NNFSession& session = _compute.get(_program.get());
    session.setImage(image);
    session.execute();
}

void
SDFGenerator::NNFSession::renderImplementation(osg::State* state)
{
    if (_L_uniform < 0)
    {
        const osg::Program::PerContextProgram* pcp = state->getLastAppliedProgramObject();
        _L_uniform = pcp->getUniformLocation(osg::Uniform::getNameID("L"));
    }

    osg::GLExtensions* ext = state->get<osg::GLExtensions>();

    // https://www.comp.nus.edu.sg/~tants/jfa/i3d06.pdf
    for (int L = _image->s() / 2; L >= 1; L /= 2)
    {
        ext->glUniform1i(_L_uniform, L);
        ext->glDispatchCompute(_image->s(), _image->t(), 1);
        ext->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
}

void
SDFGenerator::compute_nnf_on_cpu(osg::Image* buf) const
{
    // Jump-Flood algorithm for computing discrete voronoi
    // https://www.comp.nus.edu.sg/~tants/jfa/i3d06.pdf
    osg::Vec4f pixel_points_to;
    osg::Vec4f remote;
    osg::Vec4f remote_points_to;
    ImageUtils::PixelReader readBuf(buf);
    ImageUtils::PixelWriter writeBuf(buf);
    int n = buf->s();
    constexpr float NODATA = 32767;

    for (int L = n / 2; L >= 1; L /= 2)
    {
        ImageUtils::ImageIterator iter(readBuf);
        iter.forEachPixel([&]()
            {
                readBuf(pixel_points_to, iter.s(), iter.t());

                // no data at this pixel yet? skip it; there is nothing to propagate.
                if (pixel_points_to.x() == NODATA)
                    return;

                for (int s = iter.s() - L; s <= iter.s() + L; s += L)
                {
                    if (s < 0 || s >= readBuf.s()) 
                        continue;

                    remote[0] = (float)s;

                    for (int t = iter.t() - L; t <= iter.t() + L; t += L)
                    {
                        if (t < 0 || t >= readBuf.t()) 
                            continue;
                        if (s == iter.s() && t == iter.t()) 
                            continue;

                        remote[1] = (float)t;

                        // fetch the coords the remote pixel points to:
                        readBuf(remote_points_to, s, t);

                        if (remote_points_to.x() == NODATA) // remote is unset? Just copy
                        {
                            writeBuf(pixel_points_to, s, t);
                        }
                        else
                        {
                            // compare the distances and pick the closest.
                            float d_existing = distanceSquared2D(remote, remote_points_to);
                            float d_possible = distanceSquared2D(remote, pixel_points_to);

                            if (d_possible < d_existing)
                            {
                                writeBuf(pixel_points_to, s, t);
                            }
                        }
                    }
                }
            }
        );
    }
}
