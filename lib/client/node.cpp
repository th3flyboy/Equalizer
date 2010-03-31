
/* Copyright (c) 2005-2010, Stefan Eilemann <eile@equalizergraphics.com> 
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

#include <pthread.h>

#include "node.h"

#include "client.h"
#include "commands.h"
#include "config.h"
#include "frameData.h"
#include "global.h"
#include "log.h"
#include "nodeFactory.h"
#include "packets.h"
#include "pipe.h"
#include "server.h"

#ifdef AGL
#  include "aglEventHandler.h"
#elif defined WGL
#  include "wglEventHandler.h"
#endif

#include <eq/fabric/elementVisitor.h>
#include <eq/fabric/task.h>
#include <eq/net/command.h>
#include <eq/net/connection.h>
#include <eq/base/scopedMutex.h>

namespace eq
{
/** @cond IGNORE */
typedef net::CommandFunc<Node> NodeFunc;
typedef fabric::Node< Config, Node, Pipe > Super;
/** @endcond */

Node::Node( Config* parent )
#pragma warning( push )
#pragma warning( disable : 4355 )
        : transmitter( this )
#pragma warning( push )
        , _config( parent )
        , _tasks( fabric::TASK_NONE )
        , _state( STATE_STOPPED )
        , _unlockedFrame( 0 )
        , _finishedFrame( 0 )
{
    parent->_addNode( this );
    EQINFO << " New eq::Node @" << (void*)this << std::endl;
}

Node::~Node()
{
    _config->_removeNode( this );

    EQINFO << " Delete eq::Node @" << (void*)this << std::endl;
}

void Node::attachToSession( const uint32_t id, 
                            const uint32_t instanceID, 
                            net::Session* session )
{
    net::Object::attachToSession( id, instanceID, session );

    EQASSERT( _config );
    net::CommandQueue* queue = _config->getNodeThreadQueue();

    registerCommand( CMD_NODE_CREATE_PIPE, 
                     NodeFunc( this, &Node::_cmdCreatePipe ), queue );
    registerCommand( CMD_NODE_DESTROY_PIPE,
                     NodeFunc( this, &Node::_cmdDestroyPipe ), queue );
    registerCommand( CMD_NODE_CONFIG_INIT, 
                     NodeFunc( this, &Node::_cmdConfigInit ), queue );
    registerCommand( CMD_NODE_CONFIG_EXIT,
                     NodeFunc( this, &Node::_cmdConfigExit ), queue );
    registerCommand( CMD_NODE_FRAME_START,
                     NodeFunc( this, &Node::_cmdFrameStart ), queue );
    registerCommand( CMD_NODE_FRAME_FINISH,
                     NodeFunc( this, &Node::_cmdFrameFinish ), queue );
    registerCommand( CMD_NODE_FRAME_DRAW_FINISH, 
                     NodeFunc( this, &Node::_cmdFrameDrawFinish ), queue );
    registerCommand( CMD_NODE_FRAME_TASKS_FINISH, 
                     NodeFunc( this, &Node::_cmdFrameTasksFinish ), queue );
}

ClientPtr Node::getClient()
{
    EQASSERT( _config );
    return (_config ? _config->getClient() : 0);
}

ServerPtr Node::getServer()
{
    EQASSERT( _config );
    return (_config ? _config->getServer() : 0);
}

CommandQueue* Node::getNodeThreadQueue()
{
    return getClient()->getNodeThreadQueue();
}

namespace
{
template< class C >
VisitorResult _accept( C* node, NodeVisitor& visitor )
{ 
    VisitorResult result = visitor.visitPre( node );
    if( result != TRAVERSE_CONTINUE )
        return result;

    const PipeVector& pipes = node->getPipes();
    for( PipeVector::const_iterator i = pipes.begin(); i != pipes.end(); ++i )
    {
        switch( (*i)->accept( visitor ))
        {
            case TRAVERSE_TERMINATE:
                return TRAVERSE_TERMINATE;

            case TRAVERSE_PRUNE:
                result = TRAVERSE_PRUNE;
                break;
                
            case TRAVERSE_CONTINUE:
            default:
                break;
        }
    }

    switch( visitor.visitPost( node ))
    {
        case TRAVERSE_TERMINATE:
            return TRAVERSE_TERMINATE;

        case TRAVERSE_PRUNE:
            return TRAVERSE_PRUNE;
                
        case TRAVERSE_CONTINUE:
        default:
            break;
    }

    return result;
}
}

VisitorResult Node::accept( NodeVisitor& visitor )
{
    return _accept( this, visitor );
}

VisitorResult Node::accept( NodeVisitor& visitor ) const
{
    return _accept( this, visitor );
}

void Node::_addPipe( Pipe* pipe )
{
    EQASSERT( pipe->getNode() == this );
    _pipes.push_back( pipe );
}

void Node::_removePipe( Pipe* pipe )
{
    PipeVector::iterator iter = find( _pipes.begin(), _pipes.end(), pipe );
    EQASSERT( iter != _pipes.end( ))
    
    _pipes.erase( iter );
}

Pipe* Node::_findPipe( const uint32_t id )
{
    for( PipeVector::const_iterator i = _pipes.begin(); i != _pipes.end(); 
         ++i )
    {
        Pipe* pipe = *i;
        if( pipe->getID() == id )
            return pipe;
    }
    return 0;
}

net::Barrier* Node::getBarrier( const net::ObjectVersion barrier )
{
    _barriersMutex.set();
    net::Barrier* netBarrier = _barriers[ barrier.identifier ];

    if( !netBarrier )
    {
        net::Session* session = getSession();

        netBarrier = new net::Barrier;
        netBarrier->makeThreadSafe();
        EQCHECK( session->mapObject( netBarrier, barrier.identifier ));

        _barriers[ barrier.identifier ] = netBarrier;
    }
    _barriersMutex.unset();

    netBarrier->sync( barrier.version );
    return netBarrier;
}

FrameData* Node::getFrameData( const net::ObjectVersion& dataVersion )
{
    _frameDatasMutex.set();
    FrameData* frameData = _frameDatas[ dataVersion.identifier ];

    if( !frameData )
    {
        net::Session* session = getSession();
        
        frameData = new FrameData;
        frameData->makeThreadSafe();
        EQCHECK( session->mapObject( frameData, dataVersion.identifier ));

        _frameDatas[ dataVersion.identifier ] = frameData;
    }
    _frameDatasMutex.unset();

    if( frameData->getVersion() < dataVersion.version )
    {
        frameData->sync( dataVersion.version );
        frameData->update( dataVersion.version );
    }
    EQASSERT( frameData->getVersion() == dataVersion.version );

    return frameData;
}

void Node::waitInitialized() const
{
    _state.waitGE( STATE_INIT_FAILED );
}

bool Node::isRunning() const
{
    return (_state == STATE_RUNNING);
}

bool Node::configInit( const uint32_t initID )
{
#ifdef EQ_USE_MAGELLAN
#  ifdef AGL
    AGLEventHandler::initMagellan( this );
#  elif defined WGL
    WGLEventHandler::initMagellan( this );
#  else
    EQUNIMPLEMENTED;
#  endif
#endif
    return true;
}

bool Node::configExit()
{
#ifdef EQ_USE_MAGELLAN
#  ifdef AGL
    AGLEventHandler::exitMagellan( this );
#  elif defined WGL
    WGLEventHandler::exitMagellan( this );
#  else
    EQUNIMPLEMENTED;
#  endif
#endif
    return true;
}

void Node::waitFrameStarted( const uint32_t frameNumber ) const
{
    _currentFrame.waitGE( frameNumber );
}

void Node::startFrame( const uint32_t frameNumber ) 
{
    _currentFrame = frameNumber;
}

void Node::_finishFrame( const uint32_t frameNumber ) const
{
    for( PipeVector::const_iterator i = _pipes.begin(); i != _pipes.end(); ++i )
    {
        const Pipe* pipe = *i;
        EQASSERT( pipe->isThreaded() || 
                  pipe->getFinishedFrame() >= frameNumber );

        pipe->waitFrameLocal( frameNumber );
        pipe->waitFrameFinished( frameNumber );
    }
}

void Node::_frameFinish( const uint32_t frameID, const uint32_t frameNumber )
{
    frameFinish( frameID, frameNumber );
    EQLOG( LOG_TASKS ) << "---- Finished Frame --- " << frameNumber << std::endl;

    if( _unlockedFrame < frameNumber )
    {
        EQWARN << "Finished frame was not locally unlocked, enforcing unlock" 
               << std::endl;
        releaseFrameLocal( frameNumber );
    }

    if( _finishedFrame < frameNumber )
    {
        EQWARN << "Finished frame was not released, enforcing unlock" << std::endl;
        releaseFrame( frameNumber );
    }
}

void Node::releaseFrame( const uint32_t frameNumber )
{
    EQASSERTINFO( _currentFrame >= frameNumber, 
                  "current " << _currentFrame << " release " << frameNumber );

    if( _finishedFrame >= frameNumber )
        return;
    _finishedFrame = frameNumber;

    NodeFrameFinishReplyPacket packet;
    packet.frameNumber = frameNumber;

    ServerPtr server = _config->getServer();
    net::NodePtr node = server.get();
    send( node, packet );
}

void Node::releaseFrameLocal( const uint32_t frameNumber )
{
    EQASSERT( _unlockedFrame <= frameNumber );
    _unlockedFrame = frameNumber;
    
    Config* config = getConfig();
    EQASSERT( config->getNodes().size() == 1 );
    EQASSERT( config->getNodes()[0] == this );
    config->releaseFrameLocal( frameNumber );

    EQLOG( LOG_TASKS ) << "---- Unlocked Frame --- " << _unlockedFrame << std::endl;
}

void Node::frameStart( const uint32_t frameID, const uint32_t frameNumber )
{
    startFrame( frameNumber ); // unlock pipe threads
    
    switch( getIAttribute( IATTR_THREAD_MODEL ))
    {
        case ASYNC:
            // Don't wait for pipes to release frame locally, sync not needed
            releaseFrameLocal( frameNumber );
            break;

        case DRAW_SYNC:  // Sync and release in frameDrawFinish
        case LOCAL_SYNC: // Sync and release in frameTasksFinish
            break;

        default:
            EQUNIMPLEMENTED;
    }
}

void Node::frameDrawFinish( const uint32_t frameID, const uint32_t frameNumber )
{
    switch( getIAttribute( IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // No sync, release in frameStart
        case LOCAL_SYNC: // Sync and release in frameTasksFinish
            break;

        case DRAW_SYNC:
            for( PipeVector::const_iterator i = _pipes.begin();
                 i != _pipes.end(); ++i )
            {
                const Pipe* pipe = *i;
                if( pipe->getTasks() & fabric::TASK_DRAW )
                    pipe->waitFrameLocal( frameNumber );
            }
            
            releaseFrameLocal( frameNumber );
            break;

        default:
            EQUNIMPLEMENTED;
    }
}

void Node::frameTasksFinish( const uint32_t frameID, const uint32_t frameNumber)
{
    switch( getIAttribute( IATTR_THREAD_MODEL ))
    {
        case ASYNC:      // No sync, release in frameStart
        case DRAW_SYNC:  // Sync and release in frameDrawFinish
            break;

        case LOCAL_SYNC:
            for( PipeVector::const_iterator i = _pipes.begin();
                 i != _pipes.end(); ++i )
            {
                const Pipe* pipe = *i;
                if( pipe->getTasks() != fabric::TASK_NONE )
                    pipe->waitFrameLocal( frameNumber );
            }
            
            releaseFrameLocal( frameNumber );
            break;

        default:
            EQUNIMPLEMENTED;
    }
}

void Node::setErrorMessage( const std::string& message )
{
    _error = message;
}

void Node::_flushObjects()
{
    net::Session* session = getSession();

    _barriersMutex.set();
    for( BarrierHash::const_iterator i =_barriers.begin();
         i != _barriers.end(); ++ i )
    {
        net::Barrier* barrier = i->second;
        session->unmapObject( barrier );
        delete barrier;
    }
    _barriers.clear();
    _barriersMutex.unset();

    _frameDatasMutex.set();
    for( FrameDataHash::const_iterator i = _frameDatas.begin(); 
         i != _frameDatas.end(); ++ i )
    {
        FrameData* frameData = i->second;
        session->unmapObject( frameData );
        delete frameData;
    }
    _frameDatas.clear();
    _frameDatasMutex.unset();
}

void Node::TransmitThread::send( FrameData* data, net::NodePtr node, 
                                 const uint32_t frameNumber )
{
    _tasks.push( Task( data, node, frameNumber ));
}

void Node::TransmitThread::run()
{
    base::Thread::setDebugName( std::string( "Trm " ) + typeid( *_node).name());
    while( true )
    {
        const Task task = _tasks.pop();
        if( _tasks.isEmpty() && !task.node )
            return; // exit thread
        
        EQLOG( LOG_ASSEMBLY ) << "node transmit " << task.data->getID()
                              << " to " << task.node->getNodeID() << std::endl;
        task.data->transmit( task.node, task.frameNumber, _node->getID( ));
    }
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
net::CommandResult Node::_cmdCreatePipe( net::Command& command )
{
    const NodeCreatePipePacket* packet = 
        command.getPacket<NodeCreatePipePacket>();
    EQLOG( LOG_INIT ) << "Create pipe " << packet << std::endl;

    CHECK_THREAD( _nodeThread );
    EQASSERT( packet->pipeID != EQ_ID_INVALID );

    Pipe* pipe = Global::getNodeFactory()->createPipe( this );

    if( packet->threaded )
        pipe->startThread();

    _config->attachObject( pipe, packet->pipeID, EQ_ID_INVALID );

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdDestroyPipe( net::Command& command )
{
    const NodeDestroyPipePacket* packet = 
        command.getPacket<NodeDestroyPipePacket>();
    EQLOG( LOG_INIT ) << "Destroy pipe " << packet << std::endl;

    CHECK_THREAD( _nodeThread );
    Pipe* pipe = _findPipe( packet->pipeID );
    pipe->joinThread();

    _config->detachObject( pipe );
    Global::getNodeFactory()->releasePipe( pipe );

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdConfigInit( net::Command& command )
{
    CHECK_THREAD( _nodeThread );

    const NodeConfigInitPacket* packet = 
        command.getPacket<NodeConfigInitPacket>();
    EQLOG( LOG_INIT ) << "Init node " << packet << std::endl;

    _state = STATE_INITIALIZING;
    setName( packet->name );
    _tasks = packet->tasks;

    memcpy( _iAttributes, packet->iAttributes, IATTR_ALL * sizeof( int32_t ));

    _currentFrame  = packet->frameNumber;
    _unlockedFrame = packet->frameNumber;
    _finishedFrame = packet->frameNumber;

    transmitter.start();
    _error.clear();
    NodeConfigInitReplyPacket reply;
    reply.result = configInit( packet->initID );

    if( _iAttributes[ IATTR_THREAD_MODEL ] == eq::UNDEFINED )
        _iAttributes[ IATTR_THREAD_MODEL ] = eq::DRAW_SYNC;

    _state = reply.result ? STATE_RUNNING : STATE_INIT_FAILED;

    memcpy( reply.iAttributes, _iAttributes, IATTR_ALL * sizeof( int32_t ));
    send( command.getNode(), reply, _error );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdConfigExit( net::Command& command )
{
    const NodeConfigExitPacket* packet = 
        command.getPacket<NodeConfigExitPacket>();
    EQLOG( LOG_INIT ) << "Node exit " << packet << std::endl;

    CHECK_THREAD( _nodeThread );
    for( PipeVector::const_iterator i = _pipes.begin(); i != _pipes.end(); 
         ++i )
    {
        Pipe* pipe = *i;
        pipe->waitExited();
    }
    
    NodeConfigExitReplyPacket reply;
    reply.result = configExit();

    transmitter.send( 0, 0, 0 );
    transmitter.join();

    _state = STATE_STOPPED;
    _flushObjects();

    send( command.getNode(), reply );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameStart( net::Command& command )
{
    CHECK_THREAD( _nodeThread );
    const NodeFrameStartPacket* packet = 
        command.getPacket<NodeFrameStartPacket>();
    EQVERB << "handle node frame start " << packet << std::endl;

    const uint32_t frameNumber = packet->frameNumber;
    EQASSERT( _currentFrame == frameNumber-1 );

    EQLOG( LOG_TASKS ) << "----- Begin Frame ----- " << frameNumber << std::endl;

    _config->_frameStart();
    frameStart( packet->frameID, frameNumber );
    EQASSERTINFO( _currentFrame >= frameNumber, 
                  "Node::frameStart() did not start frame " << frameNumber );

    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameFinish( net::Command& command )
{
    CHECK_THREAD( _nodeThread );
    const NodeFrameFinishPacket* packet = 
        command.getPacket<NodeFrameFinishPacket>();
    EQLOG( LOG_TASKS ) << "TASK frame finish " << getName() <<  " " << packet
                       << std::endl;

    const uint32_t frameNumber = packet->frameNumber;

    _finishFrame( frameNumber );
    _frameFinish( packet->frameID, frameNumber );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameDrawFinish( net::Command& command )
{
    NodeFrameDrawFinishPacket* packet = 
        command.getPacket< NodeFrameDrawFinishPacket >();
    EQLOG( LOG_TASKS ) << "TASK draw finish " << getName() <<  " " << packet
                       << std::endl;

    frameDrawFinish( packet->frameID, packet->frameNumber );
    return net::COMMAND_HANDLED;
}

net::CommandResult Node::_cmdFrameTasksFinish( net::Command& command )
{
    NodeFrameTasksFinishPacket* packet = 
        command.getPacket< NodeFrameTasksFinishPacket >();
    EQLOG( LOG_TASKS ) << "TASK tasks finish " << getName() <<  " " << packet
                       << std::endl;

    frameTasksFinish( packet->frameID, packet->frameNumber );
    return net::COMMAND_HANDLED;
}
}


#include "../fabric/node.cpp"
template class eq::fabric::Node< eq::Config, eq::Node, eq::Pipe >;
