// ProcessMac.mm
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "Process.h"

#if defined ( APPLE_PROCESS_USE_NSTASK )

#include "Core/Env/Assert.h"
#include "Core/Math/Conversions.h"
#include "Core/Math/Constants.h"
#include "Core/Process/Atomic.h"
#include "Core/Process/Thread.h"
#include "Core/Profile/Profile.h"
#include "Core/Time/Timer.h"

// Static Data
//------------------------------------------------------------------------------

// CONSTRUCTOR
//------------------------------------------------------------------------------
Process::Process( const volatile bool * masterAbortFlag,
                  const volatile bool * abortFlag )
: m_Started( false )
    , m_Task( nil )
    , m_StdOutRead( nil )
    , m_StdErrRead( nil )
    , m_HasAborted( false )
    , m_MasterAbortFlag( masterAbortFlag )
    , m_AbortFlag( abortFlag )
{
}

// DESTRUCTOR
//------------------------------------------------------------------------------
Process::~Process()
{
    @autoreleasepool
    {
        if ( m_Started )
        {
            WaitForExit();
        }

        if ( m_Task )
        {
            [m_Task release];
            m_Task = nil;
        }
    }
}

// KillProcessTree
//------------------------------------------------------------------------------
void Process::KillProcessTree()
{
    // Kill all processes in the process group of the child process.
    kill( -m_Task.processIdentifier, SIGKILL );
}

// Spawn
//------------------------------------------------------------------------------
bool Process::Spawn( const char * executable,
                     const char * args,
                     __attribute__((unused)) const char * workingDir,
                     __attribute__((unused)) const char * environment,
                     __attribute__((unused)) bool shareHandles )
{
    PROFILE_FUNCTION

    ASSERT( !m_Started );
    ASSERT( executable );

    if ( m_MasterAbortFlag && AtomicLoadRelaxed( m_MasterAbortFlag ) )
    {
        // Once master process has aborted, we no longer permit spawning sub-processes.
        return false;
    }

    @autoreleasepool {

    m_Task = [NSTask new];

    [m_Task setLaunchPath:[NSString stringWithUTF8String:executable]];

    NSArray<NSString *> * temp = [[NSString stringWithUTF8String:args] componentsSeparatedByString:@" "];
    NSMutableArray * arguments = [[NSMutableArray alloc] init];

    for (NSUInteger i = 0; i < [temp count]; i++)
    {
        NSString * arg = ( NSString * )[temp objectAtIndex:i];

        if (/*[arg hasPrefix:@"\""] && */[arg hasSuffix:@"\""])
        {
            arg = [arg stringByReplacingOccurrencesOfString:@"\"" withString:@""];
        }
        [arguments addObject:arg];
    }

    m_StdOutRead = [NSPipe pipe];
    m_StdErrRead = [NSPipe pipe];
    [m_Task setStandardOutput:m_StdOutRead];
    [m_Task setStandardError:m_StdErrRead];

    [m_Task setArguments:arguments];

    [m_Task launch];

    [arguments release];

    m_Started = true;

    } // @autoreleasepool

    return true;
}

// IsRunning
//----------------------------------------------------------
bool Process::IsRunning() const
{
    ASSERT( m_Started );

    @autoreleasepool
    {
        return [m_Task isRunning];
    }
}

// WaitForExit
//------------------------------------------------------------------------------
int32_t Process::WaitForExit()
{
    ASSERT( m_Started );
    m_Started = false;

    @autoreleasepool
    {
        [m_Task waitUntilExit];
 
        return [m_Task terminationStatus];
    }
}

// Detach
//------------------------------------------------------------------------------
void Process::Detach()
{
    ASSERT( m_Started );
    m_Started = false;

    ASSERT( false ); // TODO:Mac Implement Process
}

// ReadAllData
//------------------------------------------------------------------------------
bool Process::ReadAllData( AutoPtr< char > & outMem, uint32_t * outMemSize,
                           AutoPtr< char > & errMem, uint32_t * errMemSize,
                           __attribute__((unused)) uint32_t timeOutMS )
{
    @autoreleasepool {

    // we'll capture into these growing buffers
    __block uint32_t outSize = 0;
    __block uint32_t errSize = 0;
    __block uint32_t outBufferSize = 0;
    __block uint32_t errBufferSize = 0;
    __block bool stdOutIsDone = false;
    __block bool stdErrIsDone = false;

    NSFileHandle * stdOutHandle = [m_StdOutRead fileHandleForReading];

    dispatch_async( dispatch_get_global_queue( DISPATCH_QUEUE_PRIORITY_LOW, 0 ), ^{
    @autoreleasepool {
        NSData * stdOutData = [stdOutHandle availableData];
        while ( [stdOutData length] > 0 )
        {
            const bool masterAbort = ( m_MasterAbortFlag && AtomicLoadRelaxed( m_MasterAbortFlag ) );
            const bool abort = ( m_AbortFlag && AtomicLoadRelaxed( m_AbortFlag ) );
            if ( abort || masterAbort )
            {
                PROFILE_SECTION( "Abort" )
                KillProcessTree();
                m_HasAborted = true;
                break;
            }

            Read( stdOutData, outMem, outSize, outBufferSize );

            stdOutData = [stdOutHandle availableData];
        }

        stdOutIsDone = true;
    }});

    NSFileHandle * stdErrHandle = [m_StdErrRead fileHandleForReading];

    dispatch_async( dispatch_get_global_queue( DISPATCH_QUEUE_PRIORITY_LOW, 0 ), ^{
    @autoreleasepool {
        NSData * stdErrData = [stdErrHandle availableData];
        while ( [stdErrData length] > 0 )
        {
            const bool masterAbort = ( m_MasterAbortFlag && AtomicLoadRelaxed( m_MasterAbortFlag ) );
            const bool abort = ( m_AbortFlag && AtomicLoadRelaxed( m_AbortFlag ) );
            if ( abort || masterAbort )
            {
                PROFILE_SECTION( "Abort" )
                KillProcessTree();
                m_HasAborted = true;
                break;
            }

            Read( stdErrData, errMem, errSize, errBufferSize );

            stdErrData = [stdErrHandle availableData];
        }

        stdErrIsDone = true;
    }});

    [m_Task waitUntilExit];

    // async tasks may still be processing the child process output, so wait for them if needed
    while ( !stdOutIsDone || !stdErrIsDone )
    {
        sched_yield();
    }

    // if owner asks for pointers, they now own the mem
    if ( outMemSize ) { *outMemSize = outSize; }
    if ( errMemSize ) { *errMemSize = errSize; }

    } // @autoreleasepool

    return true;
}

// Read
//------------------------------------------------------------------------------
void Process::Read( NSData * availableData, AutoPtr< char > & buffer, uint32_t & sizeSoFar, uint32_t & bufferSize )
{
    @autoreleasepool {

    uint32_t bytesAvail = [availableData length];

    if ( bytesAvail == 0 )
    {
        return;
    }

    // will it fit in the buffer we have?
    if ( ( sizeSoFar + bytesAvail ) > bufferSize )
    {
        // no - allocate a bigger buffer (also handles the first time with no buffer)

        // TODO:B look at a new container type (like a linked list of 1mb buffers) to avoid the wasteage here
        // The caller has to take a copy to avoid the overhead if they want to hang onto the data
        // grow buffer in at least 16MB chunks, to prevent future reallocations
        uint32_t newBufferSize = Math::Max< uint32_t >( sizeSoFar + bytesAvail, bufferSize + ( 16 * MEGABYTE ) );
        char * newBuffer = (char *)ALLOC( newBufferSize + 1 ); // +1 so we can always add a null char
        if ( buffer.Get() )
        {
            // transfer and free old buffer
            memcpy( newBuffer, buffer.Get(), sizeSoFar );
        }
        buffer = newBuffer; // will take care of deletion of old buffer
        bufferSize = newBufferSize;
        buffer.Get()[ sizeSoFar ] = '\000';
    }

    ASSERT( buffer.Get() );
    ASSERT( sizeSoFar + bytesAvail <= bufferSize ); // sanity check

    [availableData getBytes:(buffer.Get() + sizeSoFar) length:bytesAvail];

    sizeSoFar += bytesAvail;

    // keep data null char terminated for caller convenience
    buffer.Get()[ sizeSoFar ] = '\000';

    } // @autoreleasepool
}

// GetCurrentId
//------------------------------------------------------------------------------
/*static*/ uint32_t Process::GetCurrentId()
{
    return ::getpid();
}

// Terminate
//------------------------------------------------------------------------------
void Process::Terminate()
{
    kill( m_Task.processIdentifier, SIGKILL );
}

//------------------------------------------------------------------------------

#endif // APPLE_PROCESS_USE_NSTASK