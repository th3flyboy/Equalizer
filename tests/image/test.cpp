
/* Copyright (c) 2006-2009, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <test.h>

#include "../../lib/client/pluginRegistry.h"

#include <eq/plugins/compressor.h>
#include <eq/client/global.h>
#include <eq/client/image.h>
#include <eq/client/init.h>
#include <eq/client/nodeFactory.h>
#include <eq/base/clock.h>
#include <eq/base/fileSearch.h>


#include <eq/client/frame.h>    // enum Eye

#include <numeric>
#include <fstream>


// Tests the functionality and speed of the image compression.
//#define WRITE_DECOMPRESSED
//#define WRITE_COMPRESSED
#define COMPARE_RESULT

int main( int argc, char **argv )
{
    eq::NodeFactory nodeFactory;
    TEST( eq::init( argc, argv, &nodeFactory ));

    eq::StringVector images;
    eq::StringVector candidates = eq::base::fileSearch( "images", "*.rgb" );
    for( eq::StringVector::const_iterator i = candidates.begin();
        i != candidates.end(); ++i )
    {
        const std::string& filename = *i;
        const size_t decompPos = filename.find( "decomp_" );
        if( decompPos == std::string::npos )
            images.push_back( "images/" + filename );
    }

    candidates = eq::base::fileSearch( "../compositor", "Result*.rgb" );
    for( eq::StringVector::const_iterator i = candidates.begin();
        i != candidates.end(); ++i )
    {
        const std::string& filename = *i;
        const size_t decompPos = filename.find( "decomp_" );
        if( decompPos == std::string::npos )
            images.push_back( "../compositor/" + filename );
    }
    TEST( !images.empty( ));

    // Touch memory once
    eq::base::Clock clock;
    eq::Image image;
    eq::Image destImage;
    
    clock.reset();
    TEST( image.readImage( images.front(), eq::Frame::BUFFER_COLOR ));
    const float loadTime = clock.getTimef();

    std::cout.setf( std::ios::right, std::ios::adjustfield );
    std::cout.precision( 5 );
    std::cout << "Load " << images.front() << ": " << loadTime << "ms" 
              << std::endl;

    destImage.setPixelViewport( image.getPixelViewport( ));
    destImage.setPixelData( eq::Frame::BUFFER_COLOR,     
                            image.compressPixelData( eq::Frame::BUFFER_COLOR ));

    TEST( image.readImage( images.front(), eq::Frame::BUFFER_DEPTH ));
    destImage.setPixelViewport( image.getPixelViewport( ));
    destImage.setPixelData( eq::Frame::BUFFER_DEPTH,     
                            image.compressPixelData( eq::Frame::BUFFER_DEPTH ));

    std::cout << "COMPRESSOR,                            IMAGE,       SIZE, A,"
              << " COMPRESSED,     t_comp,   t_decomp" << std::endl;

    for( eq::StringVector::const_iterator i = images.begin();
         i != images.end(); ++i )
    {
        const std::string& filename = *i;
        const size_t depthPos = filename.find( "depth" );
        const eq::Frame::Buffer buffer = (depthPos == std::string::npos) ?
            eq::Frame::BUFFER_COLOR : eq::Frame::BUFFER_DEPTH;

        TEST( image.readImage( filename, buffer ));
        destImage.setPixelViewport( image.getPixelViewport( ));

        const uint8_t* data = image.getPixelPointer( buffer );
        const uint32_t size = image.getPixelDataSize( buffer );

        const std::vector<uint32_t> compressors( image.findCompressors(buffer));
        for( std::vector<uint32_t>::const_iterator j = compressors.begin();
             j != compressors.end(); ++j )
        {
            image.allocCompressor( buffer, *j );

          again:
            clock.reset();
            const eq::Image::PixelData& compressedPixels =
                image.compressPixelData( buffer );
            const float compressTime = clock.getTimef();

            TEST( compressedPixels.compressedSize.size() ==
                  compressedPixels.compressedData.size( ));
            TESTINFO( *j == compressedPixels.compressorName,
                      *j << " != " << compressedPixels.compressorName );

            uint32_t compressedSize = 0;
            if( compressedPixels.compressorName == EQ_COMPRESSOR_NONE )
                compressedSize = size;
            else 
            {
#ifdef WRITE_COMPRESSED
                std::ofstream comp( std::string( filename + ".comp" ).c_str(), 
                                    std::ios::out | std::ios::binary ); 
                TEST( comp.is_open( ));
                std::vector< void* >::const_iterator compData = 
                    compressedPixels.compressedData.begin();
#endif

                for( std::vector< uint64_t >::const_iterator k = 
                         compressedPixels.compressedSize.begin();
                     k != compressedPixels.compressedSize.end(); ++k )
                {
                    compressedSize += *k;
#ifdef WRITE_COMPRESSED
                    comp.write( reinterpret_cast<const char*>( *compData ), *k);
                    ++compData;
#endif
                }
#ifdef WRITE_COMPRESSED
                comp.close();
#endif
            }

            clock.reset();
            destImage.setPixelData( buffer, compressedPixels );
            const float decompressTime = clock.getTimef();

            std::cout  << std::setw(2) << *j << ", " << std::setw(40)
                       << filename << ", " << std::setw(10) << size << ", "
                       << !image.ignoreAlpha() << ", " << std::setw(10) 
                       << compressedSize << ", " << std::setw(10)
                       << compressTime << ", " << std::setw(10)
                       << decompressTime << std::endl;

#ifdef WRITE_DECOMPRESSED
            destImage.writeImage( eq::base::getDirname( filename ) + "/" + 
                                  "decomp_" + eq::base::getFilename( filename ),
                                  buffer );
#endif

#ifdef COMPARE_RESULT
            const uint8_t* destData = destImage.getPixelPointer( buffer );
            // last 7 pixels can be unitialized
            for( uint32_t k = 0; k < size-7; ++k )
            {
                TESTINFO( data[k] == destData[k] ||
                          ( image.ignoreAlpha() && (k%4)==3 ),
                          "got " << (int)destData[k] << " expected " <<
                          (int)data[k] << " at " << k );
            }
#endif

            if( buffer == eq::Frame::BUFFER_COLOR &&
                image.hasAlpha() && !image.ignoreAlpha( ))
            {
                image.disableAlphaUsage();
                destImage.disableAlphaUsage();
                goto again;
            }
            image.enableAlphaUsage();
            destImage.enableAlphaUsage();
        }
    }

    image.flush();
    eq::exit();
}

