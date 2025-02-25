// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/core/browser/address_phone_form_label_formatter.h"

#include "components/autofill/core/browser/label_formatter_utils.h"

namespace autofill {

AddressPhoneFormLabelFormatter::AddressPhoneFormLabelFormatter(
    const std::string& app_locale,
    ServerFieldType focused_field_type,
    const std::vector<ServerFieldType>& field_types)
    : LabelFormatter(app_locale, focused_field_type, field_types),
      form_has_street_address_(HasStreetAddress(field_types_for_labels())) {}

AddressPhoneFormLabelFormatter::~AddressPhoneFormLabelFormatter() {}

// Note that the order--phone, name, and address--in which parts of the label
// are added ensures that the label is formatted correctly for the focused
// group.
base::string16 AddressPhoneFormLabelFormatter::GetLabelForFocusedGroup(
    const AutofillProfile& profile,
    FieldTypeGroup group) const {
  std::vector<base::string16> label_parts;

  if (group != PHONE_HOME) {
    AddLabelPartIfNotEmpty(GetLabelPhone(profile, app_locale()), &label_parts);
  }

  if (group != NAME) {
    AddLabelPartIfNotEmpty(GetLabelName(profile, app_locale()), &label_parts);
  }

  if (group != ADDRESS_HOME) {
    AddLabelPartIfNotEmpty(
        GetLabelAddress(form_has_street_address_, profile, app_locale(),
                        field_types_for_labels()),
        &label_parts);
  }

  return ConstructLabelLine(label_parts);
}

}  // namespace autofill
