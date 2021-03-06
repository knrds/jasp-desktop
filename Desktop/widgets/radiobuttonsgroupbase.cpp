//
// Copyright (C) 2013-2020 University of Amsterdam
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//

#include "radiobuttonsgroupbase.h"
#include "radiobuttonbase.h"
#include <QQmlProperty>
#include <QQuickItem>
#include "log.h"


using namespace std;

RadioButtonsGroupBase::RadioButtonsGroupBase(QQuickItem* item)
	: JASPControl(item)
{
	_controlType		= ControlType::RadioButtonGroup;
}

void RadioButtonsGroupBase::setUp()
{
	JASPControl::setUp();
	QList<RadioButtonBase* > buttons;
	_getRadioButtons(this, buttons);
	QVariant buttonGroup = property("buttonGroup");

	for (RadioButtonBase* button: buttons)
	{	
		const QString& controlName = button->name();
		if (controlName.isEmpty())
			addControlError(tr("A RadioButton inside RadioButtonGroup element (name: %1) does not have any name").arg(name()));
		else
		{
			_buttons[controlName] = button;
			bool checked = button->property("checked").toBool();
			if (checked)
			{
				_checkedButton = button;
				setProperty("value", _checkedButton->name());
			}
			button->setProperty("buttonGroup", buttonGroup);
		}
	}

	if (!_checkedButton)
		Log::log() << "No checked button found in radio buttons " << name() << std::endl;

	QQuickItem::connect(this, SIGNAL(clicked(const QVariant &)), this, SLOT(radioButtonClickedHandler(const QVariant &)));
}

void RadioButtonsGroupBase::_getRadioButtons(QQuickItem* item, QList<RadioButtonBase *> &buttons) {
	for (QQuickItem* child : item->childItems())
	{
		JASPControl* jaspControl = qobject_cast<JASPControl*>(child);
		if (jaspControl)
		{
			ControlType controlType = jaspControl->controlType();
			if (controlType == ControlType::RadioButton)
				buttons.append(qobject_cast<RadioButtonBase*>(jaspControl));
			else if (controlType != ControlType::RadioButtonGroup)
				_getRadioButtons(child, buttons);
		}
		else
			_getRadioButtons(child, buttons);
	}	
}

void RadioButtonsGroupBase::bindTo(Option *option)
{
	_boundTo = dynamic_cast<OptionList *>(option);

	if (_boundTo == nullptr)
	{
		Log::log()  << "could not bind to OptionList in BoundQuickRadioButtons" << std::endl;
		return;
	}

	string value = _boundTo->value();
	if (!value.empty())
	{
		RadioButtonBase* button = _buttons[QString::fromStdString(value)];
		if (!button)
		{
			addControlError(tr("No radio button corresponding to name %1").arg(QString::fromStdString(value)));
			QStringList names = _buttons.keys();
			Log::log()  << "Known button: " << names.join(',').toStdString() << std::endl;
		}
		else
		{
			button->setProperty("checked", true);
			_checkedButton = button;
			setProperty("value", _checkedButton->name());

		}
	}
}

Option *RadioButtonsGroupBase::createOption()
{
	QString defaultValue = _checkedButton ? _checkedButton->name() : "";
	std::vector<std::string> options;
	for (QString value : _buttons.keys())
		options.push_back(value.toStdString());
	
	return new OptionList(options, defaultValue.toStdString());
}

bool RadioButtonsGroupBase::isOptionValid(Option *option)
{
	return dynamic_cast<OptionList*>(option) != nullptr;
}

bool RadioButtonsGroupBase::isJsonValid(const Json::Value &optionValue)
{
	return optionValue.type() == Json::stringValue;
}

void RadioButtonsGroupBase::radioButtonClickedHandler(const QVariant& button)
{
	QObject* objButton = button.value<QObject*>();
	if (objButton)
		objButton = objButton->parent();
	RadioButtonBase *radioButton = qobject_cast<RadioButtonBase*>(objButton);
	if (radioButton)
	{
		QString buttonName = radioButton->name();
		RadioButtonBase* foundButton = _buttons[buttonName];
		if (foundButton)
		{
			if (_checkedButton != foundButton)
			{
				if (_checkedButton)
					_checkedButton->setProperty("checked",false);
				_checkedButton = foundButton;
				setProperty("value", _checkedButton->name());

				if (_boundTo)
					_boundTo->setValue(buttonName.toStdString());
			}
		}
		else
			addControlError(tr("Radio button clicked is unknown: %1").arg(buttonName));
	}
	else
		Log::log() << "Object clicked is not a RadioButton item! Name" << objButton->objectName().toStdString();
}
