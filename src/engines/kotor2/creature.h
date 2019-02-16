/* xoreos - A reimplementation of BioWare's Aurora engine
 *
 * xoreos is the legal property of its developers, whose names
 * can be found in the AUTHORS file distributed with this source
 * distribution.
 *
 * xoreos is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * xoreos is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with xoreos. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file
 *  Creature within an area in KotOR II.
 */

#ifndef ENGINES_KOTOR2_CREATURE_H
#define ENGINES_KOTOR2_CREATURE_H

#include "src/engines/kotorbase/creature.h"

namespace Engines {

namespace KotOR2 {

class Creature : public KotOR::Creature {
public:
	/** Generate a string for the body mesh. */
	static Common::UString getBodyMeshString(KotOR::Gender gender, KotOR::Class charClass, char state = 'b');
	/** Generate a string for the body texture. */
	static Common::UString getBodyTextureString(KotOR::Gender gender, KotOR::Skin skin, KotOR::Class charClass, char state = 'b');
	/** Generate a string for the head mesh. */
	static Common::UString getHeadMeshString(KotOR::Gender gender, KotOR::Skin skin, uint32 faceId);

protected:
	void getPartModelsPC(PartModels &parts, uint32 state, uint8 textureVariation);

private:
	static uint32 transformFaceId(KotOR::Gender gender, KotOR::Skin skin, uint32 faceId);
};

} // End of namespace KotOR2

} // End of namespace Engines

#endif // ENGINES_KOTOR2_CREATURE_H
