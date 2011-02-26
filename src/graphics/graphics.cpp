/* eos - A reimplementation of BioWare's Aurora engine
 * Copyright (c) 2010-2011 Sven Hesse (DrMcCoy), Matthew Hoops (clone2727)
 *
 * The Infinity, Aurora, Odyssey and Eclipse engines, Copyright (c) BioWare corp.
 * The Electron engine, Copyright (c) Obsidian Entertainment and BioWare corp.
 *
 * This file is part of eos and is distributed under the terms of
 * the GNU General Public Licence. See COPYING for more informations.
 */

/** @file graphics/graphics.cpp
 *  The global graphics manager.
 */

#include <cmath>

#include "common/util.h"
#include "common/error.h"
#include "common/ustring.h"
#include "common/file.h"
#include "common/configman.h"
#include "common/threads.h"

#include "events/requests.h"
#include "events/events.h"

#include "graphics/graphics.h"
#include "graphics/util.h"
#include "graphics/cursor.h"
#include "graphics/fpscounter.h"
#include "graphics/renderable.h"

#include "graphics/images/decoder.h"
#include "graphics/images/screenshot.h"

DECLARE_SINGLETON(Graphics::GraphicsManager)

namespace Graphics {

static bool queueComp(Renderable *a, Renderable *b) {
	return a->getDistance() > b->getDistance();
}


GraphicsManager::GraphicsManager() {
	_ready = false;

	_needManualDeS3TC        = false;
	_supportMultipleTextures = false;

	_fullScreen = false;

	_fsaa    = 0;
	_fsaaMax = 0;

	_gamma = 1.0;

	_screen = 0;

	_fpsCounter = new FPSCounter(3);

	_cursor = 0;
	_cursorState = kCursorStateStay;

	_takeScreenshot = false;

	_hasAbandoned = false;
}

GraphicsManager::~GraphicsManager() {
	deinit();

	delete _fpsCounter;
}

void GraphicsManager::init() {
	Common::enforceMainThread();

	uint32 sdlInitFlags = SDL_INIT_TIMER | SDL_INIT_VIDEO;

	// TODO: Is this actually needed on any systems? It seems to make MacOS X fail to
	//       receive any events, too.
/*
// Might be needed on unixoid OS, but it crashes Windows. Nice.
#ifndef WIN32
	sdlInitFlags |= SDL_INIT_EVENTTHREAD;
#endif
*/

	if (SDL_Init(sdlInitFlags) < 0)
		throw Common::Exception("Failed to initialize SDL: %s", SDL_GetError());

	int  width  = ConfigMan.getInt ("width"     , 800);
	int  height = ConfigMan.getInt ("height"    , 600);
	bool fs     = ConfigMan.getBool("fullscreen", false);

	initSize(width, height, fs);
	setupScene();

	// Try to change the FSAA settings to the config value
	if (_fsaa != ConfigMan.getInt("fsaa"))
		if (!setFSAA(ConfigMan.getInt("fsaa")))
			// If that fails, set the config to the current level
			ConfigMan.setInt("fsaa", _fsaa);

	// Set the gamma correction to what the config specifies
	setGamma(ConfigMan.getDouble("gamma", 1.0));

	// Set the window title to our name
	setWindowTitle(PACKAGE_STRING);

	_ready = true;
}

void GraphicsManager::deinit() {
	Common::enforceMainThread();

	if (!_ready)
		return;

	clearVideoQueue();
	clearListContainerQueue();
	clearTextureQueue();
	clearRenderQueue();

	SDL_Quit();

	_ready = false;

	_needManualDeS3TC        = false;
	_supportMultipleTextures = false;
}

bool GraphicsManager::ready() const {
	return _ready;
}

bool GraphicsManager::needManualDeS3TC() const {
	return _needManualDeS3TC;
}

bool GraphicsManager::supportMultipleTextures() const {
	return _supportMultipleTextures;
}

int GraphicsManager::getMaxFSAA() const {
	return _fsaaMax;
}

int GraphicsManager::getCurrentFSAA() const {
	return _fsaa;
}

uint32 GraphicsManager::getFPS() const {
	return _fpsCounter->getFPS();
}

void GraphicsManager::initSize(int width, int height, bool fullscreen) {
	int bpp = SDL_GetVideoInfo()->vfmt->BitsPerPixel;
	if ((bpp != 24) && (bpp != 32))
		throw Common::Exception("Need 24 or 32 bits per pixel");

	_systemWidth  = SDL_GetVideoInfo()->current_w;
	_systemHeight = SDL_GetVideoInfo()->current_h;

	uint32 flags = SDL_OPENGL;

	_fullScreen = fullscreen;
	if (_fullScreen)
		flags |= SDL_FULLSCREEN;

	if (!setupSDLGL(width, height, bpp, flags)) {
		// Could not initialize OpenGL, trying a different bpp value

		bpp = (bpp == 32) ? 24 : 32;

		if (!setupSDLGL(width, height, bpp, flags))
			// Still couldn't initialize OpenGL, erroring out
			throw Common::Exception("Failed setting the video mode: %s", SDL_GetError());
	}

	// Initialize glew, for the extension entry points
	GLenum glewErr = glewInit();
	if (glewErr != GLEW_OK)
		throw Common::Exception("Failed initializing glew: %s", glewGetErrorString(glewErr));

	// Check if we have all needed OpenGL extensions
	checkGLExtensions();
}

bool GraphicsManager::setFSAA(int level) {
	if (!Common::isMainThread()) {
		// Not the main thread, send a request instead
		RequestMan.dispatchAndWait(RequestMan.changeFSAA(level));
		return _fsaa == level;
	}

	if (_fsaa == level)
		// Nothing to do
		return true;

	// Check if we have the support for that level
	if (level > _fsaaMax)
		return false;

	// Backup the old level and set the new level
	int oldFSAA = _fsaa;
	_fsaa = level;

	destroyContext();

	// Set the multisample level
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, (_fsaa > 0) ? 1 : 0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, _fsaa);

	uint32 flags = _screen->flags;

	// Now try to change the screen
	_screen = SDL_SetVideoMode(0, 0, 0, flags);

	if (!_screen) {
		// Failed changing, back up

		_fsaa = oldFSAA;

		// Set the multisample level
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, (_fsaa > 0) ? 1 : 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, _fsaa);
		_screen = SDL_SetVideoMode(0, 0, 0, flags);

		// There's no reason how this could possibly fail, but ok...
		if (!_screen)
			throw Common::Exception("Failed reverting to the old FSAA settings");
	}

	rebuildContext();

	return _fsaa == level;
}

int GraphicsManager::probeFSAA(int width, int height, int bpp, uint32 flags) {
	// Find the max supported FSAA level

	for (int i = 32; i >= 2; i >>= 1) {
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE    ,   8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE  ,   8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE   ,   8);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE  , bpp);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,   1);

		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, i);

		if (SDL_SetVideoMode(width, height, bpp, flags))
			return i;
	}

	return 0;
}

bool GraphicsManager::setupSDLGL(int width, int height, int bpp, uint32 flags) {
	_fsaaMax = probeFSAA(width, height, bpp, flags);

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE    ,   8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE  ,   8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE   ,   8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE  , bpp);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,   1);

	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

	_screen = SDL_SetVideoMode(width, height, bpp, flags);
	if (!_screen)
		return false;

	return true;
}

void GraphicsManager::checkGLExtensions() {
	if (!GLEW_EXT_texture_compression_s3tc) {
		warning("Your graphics card does not support the needed extension "
		        "for S3TC DXT1, DXT3 and DXT5 texture decompression");
		warning("Switching to manual S3TC DXTn decompression. "
		        "This will be slower and will take up more video memory");
		_needManualDeS3TC = true;
	}

	if (!GLEW_ARB_texture_compression) {
		warning("Your graphics card doesn't support the compressed texture API");
		warning("Switching to manual S3TC DXTn decompression. "
		        "This will be slower and will take up more video memory");

		_needManualDeS3TC = true;
	}

	if (!GLEW_ARB_multitexture) {
		warning("Your graphics card does no support applying multiple textures onto "
		        "one surface");
		warning("Eos will only use one texture. Certain surfaces may look weird");

		_supportMultipleTextures = false;
	} else
		_supportMultipleTextures = true;
}

void GraphicsManager::setWindowTitle(const Common::UString &title) {
	SDL_WM_SetCaption(title.c_str(), 0);
}

float GraphicsManager::getGamma() const {
	return _gamma;
}

void GraphicsManager::setGamma(float gamma) {
	_gamma = gamma;

	SDL_SetGamma(gamma, gamma, gamma);
}

void GraphicsManager::setupScene() {
	if (!_screen)
		throw Common::Exception("No screen initialized");

	glClearColor(0, 0, 0, 0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glViewport(0, 0, _screen->w, _screen->h);

	gluPerspective(60.0, ((GLfloat) _screen->w) / ((GLfloat) _screen->h), 1.0, 1000.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glShadeModel(GL_SMOOTH);
	glClearColor(0.0, 0.0, 0.0, 0.5);
	glClearDepth(1.0);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void GraphicsManager::lockFrame() {
	if (!Common::isMainThread())
		_frameSemaphore.lock();
}

void GraphicsManager::unlockFrame() {
	if (!Common::isMainThread())
		_frameSemaphore.unlock();
}

void GraphicsManager::abandon(TextureID *ids, uint32 count) {
	if (count == 0)
		return;

	Common::StackLock lock(_abandonMutex);

	_abandonTextures.reserve(_abandonTextures.size() + count);
	while (count-- > 0)
		_abandonTextures.push_back(*ids++);

	_hasAbandoned = true;
}

void GraphicsManager::abandon(ListID ids, uint32 count) {
	if (count == 0)
		return;

	Common::StackLock lock(_abandonMutex);

	while (count-- > 0)
		_abandonLists.push_back(ids++);

	_hasAbandoned = true;
}

void GraphicsManager::setCursor(Cursor *cursor) {
	Common::StackLock frameLock(_frameSemaphore);

	_cursor = cursor;
}

void GraphicsManager::takeScreenshot() {
	Common::StackLock frameLock(_frameSemaphore);

	_takeScreenshot = true;
}

static const Common::UString kNoTag;
const Common::UString &GraphicsManager::getObjectAt(float x, float y) {
	Common::StackLock guiFrontLock(_guiFrontObjects.mutex);

	// Sort the GUI Front objects
	_guiFrontObjects.list.sort(queueComp);

	// Map the screen coordinates to the OpenGL coordinates
	x =               x  - (_screen->w / 2.0);
	y = (_screen->h - y) - (_screen->h / 2.0);

	// Go through the GUI elements in reverse drawing order
	for (Renderable::QueueRRef obj = _guiFrontObjects.list.rbegin(); obj != _guiFrontObjects.list.rend(); ++obj) {
		// No tag, don't check
		if ((*obj)->getTag().empty())
			continue;

		// If the coordinates are "in" that object, return its tag
		if ((*obj)->isIn(x, y))
			return (*obj)->getTag();
	}

	// TODO: World objects check

	// Common::StackLock objectsLock(_objects.mutex);

	// _objects.list.sort(queueComp);

	// No object at that position
	return kNoTag;
}

void GraphicsManager::clearRenderQueue() {
	Common::StackLock objectsLock(_objects.mutex);
	Common::StackLock guiFrontLock(_guiFrontObjects.mutex);

	// Notify all objects in the queue that they have been kicked out
	for (Renderable::QueueRef obj = _objects.list.begin(); obj != _objects.list.end(); ++obj)
		(*obj)->kickedOut();

	// Notify all front GUI objects in the queue that they have been kicked out
	for (Renderable::QueueRef obj = _guiFrontObjects.list.begin(); obj != _guiFrontObjects.list.end(); ++obj)
		(*obj)->kickedOut();

	// Clear the queues
	_objects.list.clear();
	_guiFrontObjects.list.clear();
}

void GraphicsManager::renderScene() {
	Common::enforceMainThread();

	cleanupAbandoned();

	if (!_frameSemaphore.lockTry())
		return;

	Common::StackLock videosLock(_videos.mutex);

	// Switch cursor on/off
	if (_cursorState != kCursorStateStay)
		handleCursorSwitch();

	if (_fsaa > 0)
		glEnable(GL_MULTISAMPLE_ARB);

	// Clear
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glEnable(GL_TEXTURE_2D);

	if (!_videos.list.empty()) {
		// Got videos, just play those

		for (VideoDecoder::QueueRef video = _videos.list.begin(); video != _videos.list.end(); ++video) {
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glScalef(2.0 / _screen->w, 2.0 / _screen->h, 0.0);

			(*video)->render();

			if (!(*video)->isPlaying()) {
				// Finished playing, kick the video out of the queue

				(*video)->destroy();
				(*video)->kickedOut();
				video = _videos.list.erase(video);
			}

		}

		SDL_GL_SwapBuffers();

		if (_takeScreenshot) {
			Graphics::takeScreenshot();
			_takeScreenshot = false;
		}

		_fpsCounter->finishedFrame();

		if (_fsaa > 0)
			glDisable(GL_MULTISAMPLE_ARB);

		_frameSemaphore.unlock();
		return;
	}

	Common::StackLock objectsLock(_objects.mutex);
	Common::StackLock guiFrontLock(_guiFrontObjects.mutex);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glViewport(0, 0, _screen->w, _screen->h);

	gluPerspective(60.0, ((GLfloat) _screen->w) / ((GLfloat) _screen->h), 1.0, 1000.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// Notify all objects that we're now in a new frame
	for (Renderable::QueueRef obj = _objects.list.begin(); obj != _objects.list.end(); ++obj)
		(*obj)->newFrame();
	for (Renderable::QueueRef obj = _guiFrontObjects.list.begin(); obj != _guiFrontObjects.list.end(); ++obj)
		(*obj)->newFrame();

	// Sort the queues
	_objects.list.sort(queueComp);
	_guiFrontObjects.list.sort(queueComp);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// Draw normal objects
	for (Renderable::QueueRef obj = _objects.list.begin(); obj != _objects.list.end(); ++obj) {
		glPushMatrix();
		(*obj)->render();
		glPopMatrix();
	}

	glMatrixMode(GL_PROJECTION);

	glDisable(GL_DEPTH_TEST);

	// Draw the front part of the GUI
	for (Renderable::QueueRef obj = _guiFrontObjects.list.begin(); obj != _guiFrontObjects.list.end(); ++obj) {
		glLoadIdentity();
		glScalef(2.0 / _screen->w, 2.0 / _screen->h, 0.0);

		(*obj)->render();
	}

	// Draw the cursor
	if (_cursor) {
		glLoadIdentity();
		glScalef(2.0 / _screen->w, 2.0 / _screen->h, 0.0);
		//glTranslatef(- (2.0 / _screen->w), - (2.0 / _screen->h), 0.0);
		glTranslatef(- (_screen->w / 2.0), _screen->h / 2.0, 0.0);

		_cursor->render();
	}

	glEnable(GL_DEPTH_TEST);

	SDL_GL_SwapBuffers();

	if (_takeScreenshot) {
		Graphics::takeScreenshot();
		_takeScreenshot = false;
	}

	_fpsCounter->finishedFrame();

	if (_fsaa > 0)
		glDisable(GL_MULTISAMPLE_ARB);

	_frameSemaphore.unlock();
}

int GraphicsManager::getScreenWidth() const {
	if (!_screen)
		return 0;

	return _screen->w;
}

int GraphicsManager::getScreenHeight() const {
	if (!_screen)
		return 0;

	return _screen->h;
}

int GraphicsManager::getSystemWidth() const {
	return _systemWidth;
}

int GraphicsManager::getSystemHeight() const {
	return _systemHeight;
}

bool GraphicsManager::isFullScreen() const {
	return _fullScreen;
}

void GraphicsManager::rebuildTextures() {
	Common::StackLock lock(_textures.mutex);

	for (Texture::QueueRef texture = _textures.list.begin(); texture != _textures.list.end(); ++texture)
		(*texture)->rebuild();
}

void GraphicsManager::destroyTextures() {
	Common::StackLock lock(_textures.mutex);

	for (Texture::QueueRef texture = _textures.list.begin(); texture != _textures.list.end(); ++texture)
		(*texture)->destroy();
}

void GraphicsManager::clearTextureQueue() {
	Common::StackLock lock(_textures.mutex);

	for (Texture::QueueRef texture = _textures.list.begin(); texture != _textures.list.end(); ++texture) {
		(*texture)->destroy();
		(*texture)->kickedOut();
	}

	_textures.list.clear();
}

void GraphicsManager::rebuildListContainers() {
	Common::StackLock lock(_listContainers.mutex);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	for (ListContainer::QueueRef lists = _listContainers.list.begin(); lists != _listContainers.list.end(); ++lists)
		(*lists)->rebuild();
	glPopMatrix();
}

void GraphicsManager::destroyListContainers() {
	Common::StackLock lock(_listContainers.mutex);

	for (ListContainer::QueueRef lists = _listContainers.list.begin(); lists != _listContainers.list.end(); ++lists)
		(*lists)->destroy();
}

void GraphicsManager::clearListContainerQueue() {
	Common::StackLock lock(_listContainers.mutex);

	for (ListContainer::QueueRef lists = _listContainers.list.begin(); lists != _listContainers.list.end(); ++lists) {
		(*lists)->destroy();
		(*lists)->kickedOut();
	}

	_listContainers.list.clear();
}

void GraphicsManager::rebuildVideos() {
	Common::StackLock lock(_videos.mutex);

	for (VideoDecoder::QueueRef video = _videos.list.begin(); video != _videos.list.end(); ++video)
		(*video)->rebuild();
}

void GraphicsManager::destroyVideos() {
	Common::StackLock lock(_videos.mutex);

	for (VideoDecoder::QueueRef video = _videos.list.begin(); video != _videos.list.end(); ++video)
		(*video)->destroy();
}

void GraphicsManager::clearVideoQueue() {
	Common::StackLock lock(_videos.mutex);

	for (VideoDecoder::QueueRef video = _videos.list.begin(); video != _videos.list.end(); ++video) {
		(*video)->destroy();
		(*video)->kickedOut();
	}

	_videos.list.clear();
}

void GraphicsManager::destroyContext() {
	// Destroying all videos, textures and lists, since we need to
	// reload/rebuild them anyway when the context is recreated
	destroyVideos();
	destroyListContainers();
	destroyTextures();
}

void GraphicsManager::rebuildContext() {
	// Reintroduce glew to the surface
	GLenum glewErr = glewInit();
	if (glewErr != GLEW_OK)
		throw Common::Exception("Failed initializing glew: %s", glewGetErrorString(glewErr));

	// Reintroduce OpenGL to the surface
	setupScene();

	// And reload/rebuild all textures, lists and videos
	rebuildTextures();
	rebuildListContainers();
	rebuildVideos();

	// Wait for everything to settle
	RequestMan.sync();
}

void GraphicsManager::handleCursorSwitch() {
	Common::StackLock lock(_cursorMutex);

	if      (_cursorState == kCursorStateSwitchOn)
		SDL_ShowCursor(SDL_ENABLE);
	else if (_cursorState == kCursorStateSwitchOff)
		SDL_ShowCursor(SDL_DISABLE);

	_cursorState = kCursorStateStay;
}

void GraphicsManager::cleanupAbandoned() {
	if (!_hasAbandoned)
		return;

	Common::StackLock lock(_abandonMutex);

	if (!_abandonTextures.empty())
		glDeleteTextures(_abandonTextures.size(), &_abandonTextures[0]);

	for (std::list<ListID>::iterator l = _abandonLists.begin(); l != _abandonLists.end(); ++l)
		glDeleteLists(*l, 1);

	_abandonTextures.clear();
	_abandonLists.clear();

	_hasAbandoned = false;
}

void GraphicsManager::toggleFullScreen() {
	setFullScreen(!_fullScreen);
}

void GraphicsManager::setFullScreen(bool fullScreen) {
	if (_fullScreen == fullScreen)
		// Nothing to do
		return;

	if (!Common::isMainThread()) {
		// Not the main thread, send a request instead
		RequestMan.dispatchAndWait(RequestMan.fullscreen(fullScreen));
		return;
	}

	destroyContext();

	// Save the flags
	uint32 flags = _screen->flags;

	// Now try to change modes
	_screen = SDL_SetVideoMode(0, 0, 0, flags ^ SDL_FULLSCREEN);

	// If we could not go full screen, revert back.
	if (!_screen)
		_screen = SDL_SetVideoMode(0, 0, 0, flags);
	else
		_fullScreen = fullScreen;

	// There's no reason how this could possibly fail, but ok...
	if (!_screen)
		throw Common::Exception("Failed going to fullscreen and then failed reverting.");

	rebuildContext();
}

void GraphicsManager::toggleMouseGrab() {
	// Same as ScummVM's OSystem_SDL::toggleMouseGrab()
	if (SDL_WM_GrabInput(SDL_GRAB_QUERY) == SDL_GRAB_OFF)
		SDL_WM_GrabInput(SDL_GRAB_ON);
	else
		SDL_WM_GrabInput(SDL_GRAB_OFF);
}

void GraphicsManager::setScreenSize(int width, int height) {
	if ((width == _screen->w) && (height == _screen->h))
		// No changes, nothing to do
		return;

	if (!Common::isMainThread()) {
		// Not the main thread, send a request instead
		RequestMan.dispatchAndWait(RequestMan.resize(width, height));
		return;
	}

	// Save properties
	uint32 flags     = _screen->flags;
	int    bpp       = _screen->format->BitsPerPixel;
	int    oldWidth  = _screen->w;
	int    oldHeight = _screen->h;

	destroyContext();

	// Now try to change modes
	_screen = SDL_SetVideoMode(width, height, bpp, flags);

	if (!_screen) {
		// Could not change mode, revert back.
		_screen = SDL_SetVideoMode(oldWidth, oldHeight, bpp, flags);
	}

	// There's no reason how this could possibly fail, but ok...
	if (!_screen)
		throw Common::Exception("Failed changing the resolution and then failed reverting.");

	rebuildContext();

	if ((oldWidth != _screen->w) || (oldHeight != _screen->h))
		// Tell the gui front objects that the resolution changed
		for (Renderable::QueueRef obj = _guiFrontObjects.list.begin(); obj != _guiFrontObjects.list.end(); ++obj)
			(*obj)->changedResolution(oldWidth, oldHeight, _screen->w, _screen->h);
}

Texture::Queue &GraphicsManager::getTextureQueue() {
	return _textures;
}

Renderable::Queue &GraphicsManager::getObjectQueue() {
	return _objects;
}

Renderable::Queue &GraphicsManager::getGUIFrontQueue() {
	return _guiFrontObjects;
}

ListContainer::Queue &GraphicsManager::getListContainerQueue() {
	return _listContainers;
}

VideoDecoder::Queue &GraphicsManager::getVideoQueue() {
	return _videos;
}

Queueable<Renderable>::Queue &GraphicsManager::getRenderableQueue(RenderableQueue queue) {
	if      (queue == kRenderableQueueObject)
		return getObjectQueue();
	else if (queue == kRenderableQueueGUIFront)
		return getGUIFrontQueue();
	else
		throw Common::Exception("Unknown queue");
}

void GraphicsManager::showCursor(bool show) {
	Common::StackLock lock(_cursorMutex);

	_cursorState = show ? kCursorStateSwitchOn : kCursorStateSwitchOff;
}

} // End of namespace Graphics
