#include "GraphicsSystem.h"
#include "PRISMGameState.h"

#include "OgreRoot.h"
#include "OgreWindow.h"
#include "OgreConfigFile.h"

#include "MainEntryPointHelper.h"
#include "System/MainEntryPoints.h"

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#endif

namespace Demo
{
    class PRISMGraphicsSystem : public GraphicsSystem
    {
        virtual Ogre::CompositorWorkspace* setupCompositor()
        {
            Ogre::CompositorManager2 *compositorManager = mRoot->getCompositorManager2();
            return compositorManager->addWorkspace( mSceneManager, mRenderWindow->getTexture(), mCamera,
                                                    "Tutorial_1InitializationWorkspace", true );
        }

    public:
        PRISMGraphicsSystem( GameState *gameState ) :
            GraphicsSystem( gameState )
        {
        }
    };

    void MainEntryPoints::createSystems( GameState **outGraphicsGameState,
                                         GraphicsSystem **outGraphicsSystem,
                                         GameState **outLogicGameState,
                                         LogicSystem **outLogicSystem )
    {
        PRISMGameState *gameState = new PRISMGameState(
        "P.R.I.S.M Project - Initialized with OgreNext.
"
        "This is the start of something great." );

        PRISMGraphicsSystem *graphicsSystem = new PRISMGraphicsSystem( gameState );

        gameState->_notifyGraphicsSystem( graphicsSystem );

        *outGraphicsGameState = gameState;
        *outGraphicsSystem = graphicsSystem;
    }

    void MainEntryPoints::destroySystems( GameState *graphicsGameState,
                                          GraphicsSystem *graphicsSystem,
                                          GameState *logicGameState,
                                          LogicSystem *logicSystem )
    {
        delete graphicsSystem;
        delete graphicsGameState;
    }

    void MainEntryPoints::postInitializeSystems( SceneManager *sceneManager )
    {
    }

    void MainEntryPoints::setupResources(void)
    {
        GraphicsSystem::setupResources();

        Ogre::ConfigFile cf;
        cf.load(mResourcePath + "resources2.cfg");

        Ogre::String dataPath = cf.getSetting( "DoNotModify", "Config", "DataPath" );
        if( dataPath.empty() )
            dataPath = "../../../Samples/Media/";

        Ogre::ResourceGroupManager &resourceGroupManager = Ogre::ResourceGroupManager::getSingleton();
        resourceGroupManager.addResourceLocation( dataPath + "2.0/scripts/Compositors",
                                                  "FileSystem", "General" );
        resourceGroupManager.addResourceLocation( dataPath + "models",
                                                  "FileSystem", "General" );
        resourceGroupManager.addResourceLocation( dataPath + "materials/Common",
                                                  "FileSystem", "General" );
    }
}

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
int WINAPI WinMain( HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpszCmdLine, int nCmdShow )
#else
int main( int argc, const char *argv[] )
#endif
{
    return Demo::MainEntryPointHelper::mainEntryPoint( NULL, NULL );
}
