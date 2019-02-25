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
 *  An item in a Neverwinter Nights 2 area.
 */

#include "src/common/scopedptr.h"

#include "src/aurora/gff3file.h"
#include "src/aurora/2dafile.h"
#include "src/aurora/2dareg.h"

#include "src/engines/aurora/util.h"

#include "src/engines/nwn2/item.h"

namespace Engines {

namespace NWN2 {

Item::Item(const Aurora::GFF3Struct &item) : Object(kObjectTypeItem),
	_stackSize(1), _droppable(true), _identified(true), _pickpocketable(true) {

	load(item);
}

Item::~Item() {
}

bool Item::getDroppableFlag() const {
	return _droppable;
}

bool Item::getIdentified() const {
	return _identified;
}

bool Item::getItemCursedFlag() const {
	return _cursed;
}

bool Item::getPickpocketableFlag() const {
	return _pickpocketable;
}

bool Item::getPlotFlag() const {
	return _plot;
}

bool Item::getStolenFlag() const {
	return _stolen;
}

void Item::setDroppableFlag(bool droppable) {
	_droppable = droppable;
}

void Item::setIdentified(bool identified) {
	_identified = identified;
}

void Item::setItemCursedFlag(bool cursed) {
	_cursed = cursed;
}

void Item::setPickpocketableFlag(bool pickpocketable) {
	_pickpocketable = pickpocketable;
}

void Item::setPlotFlag(bool plotFlag) {
	_plot = plotFlag;
}

void Item::setStolenFlag(bool stolen) {
	_stolen = stolen;
}

ItemType Item::getBaseItemType() const {
	return _baseItem;
}

uint32 Item::getItemIcon() const {
	return _icon;
}

uint16 Item::getItemStackSize() const {
	return _stackSize;
}

void Item::setItemIcon(uint32 icon) {
	// Check if the icon is in range
	const Aurora::TwoDAFile &icons = TwoDAReg.get2DA("nwn2_icons");
	size_t rows = icons.getRowCount();
	if (icon >= rows)
		return;

	// Check for a non-null icon name
	const Aurora::TwoDARow &row = TwoDAReg.get2DA("nwn2_icons").getRow((size_t) icon);
	if (row.getString("ICON") == "")
		return;

	_icon = icon;
}

void Item::setItemStackSize(uint16 stackSize) {
	// Check if the value is in the allowed range
	const Aurora::TwoDARow &row = TwoDAReg.get2DA("baseitems").getRow((size_t) _baseItem);
	uint32 maxStack = (uint32) row.getInt("Stacking");
	if (stackSize == 0 || stackSize > maxStack)
		return;

	_stackSize = stackSize;
}

void Item::load(const Aurora::GFF3Struct &item) {
	Common::UString temp = item.getString("TemplateResRef");

	Common::ScopedPtr<Aurora::GFF3File> uti;
	if (!temp.empty())
		uti.reset(loadOptionalGFF3(temp, Aurora::kFileTypeUTD, MKTAG('U', 'T', 'I', ' ')));

	load(item, uti ? &uti->getTopLevel() : 0);
}

void Item::load(const Aurora::GFF3Struct &instance, const Aurora::GFF3Struct *blueprint) {

	if (blueprint)
		loadProperties(*blueprint); // Blueprint
	loadProperties(instance); // Instance
}

void Item::loadProperties(const Aurora::GFF3Struct &gff) {

	// Object properties
	_tag = gff.getString("Tag", _tag);
	_name = gff.getString("LocalizedName", _name);
	_description = gff.getString("Description", _description);

	// Item properties
	_icon = gff.getUint("Icon", _icon);
	_cost = gff.getUint("Cost", _cost);
	_modifyCost = gff.getSint("ModifyCost", _modifyCost);
	_baseItem = (ItemType) gff.getUint("BaseItem", (uint) _baseItem);
	_stackSize = gff.getUint("StackSize", _stackSize);

	// Booleans
	_plot = gff.getBool("Plot", _plot);
	_cursed = gff.getBool("Cursed", _cursed);
	_stolen = gff.getBool("Stolen", _stolen);
	_droppable = gff.getBool("Dropable", _droppable);
	_identified = gff.getBool("Identified", _identified);
	_pickpocketable = gff.getBool("Pickpocketable", _pickpocketable);
}

} // End of namespace NWN2

} // End of namespace Engines
