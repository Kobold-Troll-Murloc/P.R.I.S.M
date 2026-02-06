#include "TutorialGameState.h"

namespace Demo
{
    class PRISMGameState : public TutorialGameState
    {
    public:
        PRISMGameState( const Ogre::String &helpDescription );
        virtual void createScene01(void);
    };
}
