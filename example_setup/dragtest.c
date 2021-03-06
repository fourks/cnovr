//This was one of the first programs written when developing cnovr.
//Do not use this as a template for "good coding" moving forward.

#include <cnovrtcc.h>
#include <cnovrparts.h>
#include <cnovrfocus.h>
#include <cnovr.h>
#include <cnovrutil.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <chew.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int handle;

cnovr_shader * shader;

/////////////////////////////////////////////////////////////////////////////////
// Display stuff

og_thread_t gtt;

int frame_in_buffer = -1;
int need_texture = 1;
XShmSegmentInfo shminfo;
Display * localdisplay;
int quitting;

#define MAX_DRAGGABLE_WINDOWS 16

struct DraggableWindow
{
	Window windowtrack;
	XImage * image;
	int width, height;
	int lastwidth, lastheight;
	cnovr_model * model;
	//cnovr_texture * texture;
	GLuint textureid;
	GLuint pboid;
	uint8_t * mapptr;
	cnovrfocus_capture focusblock;
	int ptrx, ptry;
};


// Static components
struct staticstore
{
	int initialized;
	int windowpidof[MAX_DRAGGABLE_WINDOWS];
	cnovr_pose modelpose[MAX_DRAGGABLE_WINDOWS];
	float uniformset[MAX_DRAGGABLE_WINDOWS*4];
} * store;


struct DraggableWindow dwindows[MAX_DRAGGABLE_WINDOWS];
int current_window_check;


Window GetWindowIdBySubstring( const char * windownamematch, const char * windowmatchexename, int matchpid /*-1 to ignore*/, int * pidout );

int AllocateNewWindow( const char * name, const char * matchingwindowpname, int pidmatch )
{
	int pid;
	Window wnd = GetWindowIdBySubstring( name, matchingwindowpname, pidmatch, &pid );
	printf( "GOT WINDOW: %d / %d\n", wnd, pid );
	if( !wnd )
	{
		printf( "Can't find window name %s\n", name );
		return -1;
	}
	int i;
	struct DraggableWindow * dw;
	for( i = 0; i < MAX_DRAGGABLE_WINDOWS; i++ )
	{
		dw = &dwindows[i];
		if( !dw->windowtrack )
		{
			break;
		}
	}
	if( i == MAX_DRAGGABLE_WINDOWS )
	{
		printf( "Can't allocate another draggable window\n" );
		return -2;
	}

	dw->windowtrack = wnd;
	dw->width = 0;
	dw->height = 0;
	dw->image = 0;
	store->windowpidof[i] = pid;
	//pose_make_identity( store->modelpose + i );

	printf( "SET INTERACTABLE: %p %p\n", dw->model, &dw->focusblock );
	CNOVRModelSetInteractable( dw->model, &dw->focusblock );

	return i;
}

int Xerrhandler(Display * d, XErrorEvent * e)
{
	printf( "XShmGetImage error\n" );
    return 0;
}


Window GetWindowIdBySubstring( const char * windownamematch, const char * windowmatchexename, int matchpid, int * pidout )
{
	Window ret  = 0;

    Window rootWindow = RootWindow( localdisplay, DefaultScreen(localdisplay));
	if( windownamematch && strcmp( windownamematch, "ROOTWINDOW" ) == 0 )
	{
		return rootWindow;
	}
    Atom atom = XInternAtom(localdisplay, "_NET_CLIENT_LIST", true);
    Atom atomGetPid = XInternAtom(localdisplay, "_NET_WM_PID", true);
    Atom actualType;
    Atom actualTypeGetPid;
    int format;
	int formatGetPid;
    unsigned long numItems, numItems_pid;
    unsigned long bytesAfter;

    Window * list = 0;    

    int status = XGetWindowProperty(localdisplay, rootWindow, atom, 0L, (~0L), false,
        AnyPropertyType, &actualType, &format, &numItems, &bytesAfter, (char**)&list );

	uint8_t * pidata = 0;
    char * windowName = 0;
    
    if (status >= Success && numItems) {
		printf( "There are %d windows\n", numItems );
        for (int i = 0; i < numItems; ++i) {
			if( !list[i] ) continue;
			pidata = 0;
			windowName = 0;
			unsigned long bytesAfter_pid;
         	int namestatus  = XFetchName(localdisplay, list[i], &windowName);

		 	int pidstatus = XGetWindowProperty(localdisplay, list[i], atomGetPid,
				0L, 1024, false, AnyPropertyType, &actualTypeGetPid, &formatGetPid,
				&numItems_pid, &bytesAfter_pid, (char**)&pidata );

			int pid = (pidata)?( pidata[1] * 256 + pidata[0] ) : 0;

	        if ( namestatus >= Success && windowName ) {
				printf( "%s\n", windowName );
			}

			if( windownamematch )
			{
		        if ( namestatus >= Success && windowName ) {
					if( strstr( windowName, windownamematch ) != 0 )
					{
						goto success;
					}
		        }
			}

			if( pid == matchpid && matchpid >= 0 )
			{
				goto success;
			}

	
			char stp[128];
			char linkprop[1024];
			sprintf( stp, "/proc/%d/exe", pid );
			int rlp = readlink( stp, linkprop, sizeof( linkprop ) - 1 );

			if( rlp > 0 )
			{
				linkprop[rlp] = 0;
				if( windowmatchexename )
				{
					if( strstr( linkprop, windowmatchexename ) != 0 )
					{
						goto success;
					}
				}
			}

		    XFree(pidata);
		    XFree(windowName);
			continue;
success:
			ret = list[i];
			if( pidout ) *pidout = pid;
			XFree(pidata);
			XFree(windowName);
			break;
        }
    }
    XFree(list);

	return ret;
}

void * GetTextureThread( void * v )
{
	XInitThreads();
	localdisplay = XOpenDisplay(NULL);
	XSetErrorHandler(Xerrhandler);


	shminfo.shmid = shmget(IPC_PRIVATE,
		2048*4 * 2048,
		IPC_PRIVATE | IPC_EXCL | IPC_CREAT|0777);

	shminfo.shmaddr = shmat(shminfo.shmid, 0, 0);
	shminfo.readOnly = False;
	XShmAttach(localdisplay, &shminfo);


//	ListWindows();
//	AllocateNewWindow( 0, "/firefox", -1 );
	AllocateNewWindow( "Frame Timing", 0, -1 );
	AllocateNewWindow( ": ~/git/cnovr", 0, -1 );
	AllocateNewWindow( 0, "/xed", -1 );
	AllocateNewWindow( "ROOTWINDOW", 0, -1 );

	while( !quitting )
	{
		if( !need_texture ) { OGUSleep( 2000 ); continue; }
		current_window_check = (current_window_check+1)%MAX_DRAGGABLE_WINDOWS;
		struct DraggableWindow * dw = &dwindows[current_window_check];
		if( !dw->windowtrack ) { if( current_window_check == 0 ) OGUSleep( 2000 ); continue; }

		XWindowAttributes attribs;
		XGetWindowAttributes(localdisplay, dw->windowtrack, &attribs);
		int taint = 0;
		if( attribs.width != dw->width ) taint = 1;
		if( attribs.height != dw->height ) taint = 1;
		int width = dw->width = attribs.width;
		int height = dw->height = attribs.height;

		if( taint  || !dw->image)
		{
			if( dw->image ) XDestroyImage( dw->image );
			dw->image = XShmCreateImage( localdisplay, 
				attribs.visual, attribs.depth,
				ZPixmap, NULL, &shminfo, width, height); 

			//shminfo.shmaddr = dw->mapptr;
			dw->image->data = shminfo.shmaddr;
		}
	 
		int rx, ry, wx, wy, mask;
		Window windowreturned;
		int ptrresult = XQueryPointer( localdisplay, dw->windowtrack, &windowreturned, &windowreturned, &rx, &ry, &wx, &wy, &mask );
		if( ptrresult )
		{
			dw->ptrx = wx;
			dw->ptry = wy;
		}
//        if (ptrresult == True) {
 //           printf( "%d  %d %d %d %d %d\n", ptrresult, rx, ry, wx, wy, mask );
  //      }
	//	printf( "%d %d\n", dw->windowtrack, dw->image );
		double ds = OGGetAbsoluteTime();
		XShmGetImage(localdisplay, dw->windowtrack, dw->image, 0, 0, AllPlanes);
		double part1 = OGGetAbsoluteTime() - ds;
		//printf( "Got Frame %d %p %p %p\n", current_window_check, dw->image, dw->image->data, *(int32_t*)(&(dw->image->data[0])) );
		if( dw->image && dw->image->data && (void*)(&(dw->image->data[0])) != (void*)(-1) )
		{
			need_texture = 0;
			frame_in_buffer = current_window_check;
		}
		//No way we'd need to be woken up faster than this.
		OGUSleep( 2000 );
	}
	printf( "Closing display\n" );
	XCloseDisplay( localdisplay );
	printf( "Closing thraed\n" );
	return 0;
}



void PreRender()
{
}

void Update()
{
}

void PostRender()
{
	if( frame_in_buffer >= 0 )
	{
		struct DraggableWindow * dw = &dwindows[frame_in_buffer];
		if( dw->lastwidth != dw->width || dw->lastheight != dw->height )
		{
			cnovr_vbo * geo = dw->model->pGeos[0];
			cnovr_vbo * geoextra = dw->model->pGeos[3];
			int x, y, side;
			int rows = 1;
			int cols = 1;
			float aspect = dw->width / ((float)dw->height + 1.0);
			for( side = 0; side < 2; side++ )
			for( y = 0; y <= rows; y++ )
			for( x = 0; x <= cols; x++ )
			{
				float * stage = &geo->pVertices[((x + y*2) * 3)+side*12];
				stage[0] = aspect * (side?(x/(float)rows):(1-x/(float)rows));
				stage[1] = y/(float)cols;
				stage[0] = (stage[0] - 0.5)*2.0;
				stage[1] = (stage[1] - 0.5)*2.0;
				stage[2] = 0;

				stage = &geoextra->pVertices[((x + y*2) * 4)+side*16];
				//printf( "Extra: %f %f %f %f %f\n", stage[0], stage[1], stage[2], stage[3] );
				CNOVRVBOTaint( geo );
//				CNOVRVBOTaint( geoextra );
				dw->lastwidth = dw->width;
				dw->lastheight = dw->height;
			}

			//if( dw->mapptr )
			//CNOVRTextureLoadDataNow( dw->texture, dw->width, dw->height, 4, 0, dw->mapptr, 0 );
			GLuint texid = dw->textureid;
//			CNOVRTextureLoadDataNow( dw->texture, dw->width, dw->height, 4, 0, dw->mapptr, 1 );
		    glBindTexture( GL_TEXTURE_2D, texid );
		    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, dw->width, dw->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, dw->mapptr );//dw->image->data );
		} 
		else
		{
			printf( "Trying to side-load %d %d %p\n", dw->width, dw->height, dw->mapptr );

/*
	glTexImage2D( GL_TEXTURE_2D,
		0,
		t->nInternalFormat,
		t->width,
		t->height,
		0,
		t->nFormat,
		t->nType,
		t->data );
	glBindTexture( GL_TEXTURE_2D, 0 );
	OGUnlockMutex( t->mutProtect );*/

			GLuint texid = dw->textureid;
//			CNOVRTextureLoadDataNow( dw->texture, dw->width, dw->height, 4, 0, dw->mapptr, 1 );
		    glBindTexture( GL_TEXTURE_2D, texid );
		    glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, dw->width, dw->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, dw->mapptr );//dw->image->data );//dw->mapptr );

		printf( "!!! %5d %5d %9d %d %p\n", dw->width, dw->height,*(int32_t*)(&(dw->image->data[0])), texid, dw->mapptr[0] );

	//		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	//	    glBindTexture( GL_TEXTURE_2D, texid );
	//	    glBindBuffer( GL_PIXEL_UNPACK_BUFFER, dw->pboid );
	//	    glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, dw->width, dw->height, GL_RGBA, GL_UNSIGNED_BYTE, 0 );
	//	    glBindTexture( GL_TEXTURE_2D, 0 );
			// = glMapBuffer( GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY );
	//		printf( "MAP: %p\n", dw->mapptr );
	//		glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);


			store->uniformset[frame_in_buffer*4+0] = dw->width;
			store->uniformset[frame_in_buffer*4+1] = dw->height;
			need_texture = 1;
			frame_in_buffer = -1;
		}
	}
}

void Render()
{
	int i;
	for( i = 0; i < MAX_DRAGGABLE_WINDOWS; i++ )
	{
		struct DraggableWindow * dw = &dwindows[i];
		//float aspect = dw->width / ((float)dw->height + 1.0);
		if( dw->width > 0 && dw->height > 0 )
		{
			store->uniformset[i*4+2] = dw->ptrx / (float)dw->width;
			store->uniformset[i*4+3] = dw->ptry / (float)dw->height;
		}
	}

	glEnable( GL_TEXTURE_2D );
	CNOVRRender( shader );
	glUniform4fv( 19, MAX_DRAGGABLE_WINDOWS*4, store->uniformset ); glGetError(); //glGetError ignores the error if we aren't examining uniform #17
	for( i = 0; i < MAX_DRAGGABLE_WINDOWS; i++ )
	{
		struct DraggableWindow * dw = &dwindows[i];
		if( dw->windowtrack && store->windowpidof[i] >= 0 )
		{
			glBindTexture( GL_TEXTURE_2D, dw->textureid );
			CNOVRRender( dw->model );
		}
	}
}


int DockableWindowFocusEvent( int event, cnovrfocus_capture * cap, cnovrfocus_properties * prop, int buttoninfo )
{
	CNOVRModelHandleFocusEvent( cap->opaque, prop, event, buttoninfo );
	if( event == CNOVRF_LOSTFOCUS )
	{
		CNOVRNamedPtrSave( "draggablewindowsdata" );
	}
	return 0;
}



void prerender_startup( void * tag, void * opaquev )
{
	int i;
	for( i = 0; i < MAX_DRAGGABLE_WINDOWS; i++ )
	{
		struct DraggableWindow * dw = &dwindows[i];

		//XXX TODO: Wouldn't it be cool if we could make this a single render call?
		//Not sure how we would handle the textures, though.
		dw->model = CNOVRModelCreate( 0, 4, GL_TRIANGLES );
		cnovr_point4d extradata = { i, 0, 0, 0 };
		CNOVRModelAppendMesh( dw->model, 1, 1, 1, (cnovr_point3d){ 1, 1, 0 }, 0, &extradata );
		CNOVRModelAppendMesh( dw->model, 1, 1, 1, (cnovr_point3d){ -1, 1, 0. }, 0, &extradata );
		if( !store->initialized )
		{
			pose_make_identity( store->modelpose + i );
			store->windowpidof[i] = -1;
		}
		dw->model->pose = store->modelpose + i;
		dw->focusblock.tag = 0;
		dw->focusblock.opaque = dw->model;
		dw->focusblock.cb = DockableWindowFocusEvent;
		dw->mapptr = malloc( 2048*2048*4 );
		memset( dw->mapptr, 0xaa, 2048*2048*4 );
		//dw->texture = CNOVRTextureCreate( 0, 0, 0 );
	//	glGenBuffers( 1, &dw->pboid );
	//	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, dw->pboid ); //bind pbo
	//	glBufferData(GL_PIXEL_UNPACK_BUFFER, 2048*2048*4, NULL, GL_STREAM_DRAW);
	//	dw->mapptr = glMapBufferRange( GL_PIXEL_UNPACK_BUFFER, 0, 2048*2048*4, GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);
	//	printf( "MAPPED PTR: %p %d\n", dw->mapptr, glGetError() );

		glGenTextures( 1, &dw->textureid );
	}

	store->initialized = 1;

	gtt = OGCreateThread( GetTextureThread, 0 );

	shader = CNOVRShaderCreate( "draggablewindow" );

	CNOVRListAdd( cnovrLRender, &handle, Render );
	CNOVRListAdd( cnovrLPrerender, &handle, PreRender );
	CNOVRListAdd( cnovrLUpdate, &handle, Update );
	CNOVRListAdd( cnovrLPostRender, &handle, PostRender );
}

void start( const char * identifier )
{
	printf( "Start\n" );
	store = CNOVRNamedPtrData( "draggablewindowsdata", 0, 1024 );
//	store->initialized = 0;

	printf( "Store: %p\n", store );
	printf( "Dockables start %s(%p)\n", identifier, identifier );
	CNOVRJobTack( cnovrQPrerender, prerender_startup, 0, 0, 0 );
	printf( "Dockables start OK %s\n", identifier );

}

void stop( const char * identifier )
{
	CNOVRListDeleteTCCTag( 0 );

	quitting = 1;
	printf( "Joining\n" );
	if( gtt ) 
	{
		printf( "actually joining\n" );
		OGJoinThread( gtt );
	}
	printf( "Stopped\n" );

	CNOVRListDeleteTag( &handle );
	printf( "Dockables Destroying: %p %p\n" ,shader );
	int i;
	for( i = 0; i < MAX_DRAGGABLE_WINDOWS; i++ )
	{
		struct DraggableWindow * dw = &dwindows[i];
		CNOVRDelete( dw->model );
		//CNOVRDelete( dw->texture );
		printf( "Deleting: %d\n", dw->textureid );
	//	glDeleteTextures( 1, &dw->textureid );
	}
	

	//Freeing shmem
	if( shminfo.shmid >= 0 )
	{
		shmdt(shminfo.shmaddr);
		/* 'remove' shared memory segment */
		shmctl(shminfo.shmid, IPC_RMID, NULL);
	}


	CNOVRDelete( shader );
	printf( "Dockables End stop\n" );
}

