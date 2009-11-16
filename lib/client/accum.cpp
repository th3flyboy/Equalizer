
/* Copyright (c) 2009, Stefan Eilemann <eile@equalizergraphics.com>
 *                   , Sarah Amsellem <sarah.amsellem@gmail.com>
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

#include "accumBufferObject.h"
#include "accum.h"

namespace eq
{

Accum::Accum( GLEWContext* const glewContext )
    : _glewContext( glewContext )
    , _width( 0 )
    , _height( 0 )
    , _abo( 0 )
    , _numSteps( 0 )
    , _totalSteps( 0 )
{
}

Accum::~Accum()
{
    exit();
}

bool Accum::init( const PixelViewport& pvp, GLuint textureFormat )
{
    if( usesFBO( ))
    {
        _abo = new AccumBufferObject( _glewContext );
        if( _abo->init( pvp.w, pvp.h, textureFormat ))
            return true;
            
        delete _abo;
        _abo = 0;
        return false;
    }

    if( _totalSteps == 0 )
       _totalSteps = getMaxSteps();

    _width = pvp.w;
    _height = pvp.h;

    return true;
}

void Accum::exit()
{
    clear();

    if( _abo )
        _abo->exit();

    delete _abo;
    _abo = 0;
}

void Accum::clear()
{
    _numSteps = 0;
}

bool Accum::resize( const int width, const int height )
{
    if( usesFBO( ))
    {
        const PixelViewport& pvp = _abo->getPixelViewport();
        if( pvp.w == width && pvp.h == height )
            return false;

        return _abo->resize( width, height );
    }
    
    if( width != _width || height != _height )
    {
        _width = width;
        _height = height;
        return true;
    }

    return false;
}

void Accum::accum()
{
/**
 * This is the only working implementation on MacOS found at the moment.
 * The glAccum function seems to be implemented differently.
 */
    if( _abo )
    {
        if( _numSteps == 0 )
            _abo->load( 1.0f );
        else
            _abo->accum( 1.0f );
    }
    else
    {
        if( _numSteps == 0 )
#ifdef Darwin
            glAccum( GL_LOAD, 1.0f/_totalSteps );
#else
            glAccum( GL_LOAD, 1.0f );
#endif
        else
#ifdef Darwin
            glAccum( GL_ACCUM, 1.0f/_totalSteps );
#else
            glAccum( GL_ACCUM, 1.0f );
#endif
    }

    ++_numSteps;
}

void Accum::display()
{
    const float factor = 1.0f/_numSteps;

    if( _abo )
        _abo->display( factor );
    else
    {
#ifdef Darwin
        glAccum( GL_RETURN, static_cast<float>( _totalSteps )/_numSteps );
#else
        glAccum( GL_RETURN, factor );
#endif
    }
}

uint32_t Accum::getMaxSteps() const
{
    if( usesFBO( ))
        return 256;
    
    GLint accumBits;
    glGetIntegerv( GL_ACCUM_RED_BITS, &accumBits );

    return accumBits >= 16 ? 256 : 0;
}
        
}

