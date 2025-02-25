// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/core/browser/address_email_form_label_formatter.h"

#include "components/autofill/core/browser/label_formatter_utils.h"

namespace autofill {

AddressEmailFormLabelFormatter::AddressEmailFormLabelFormatter(
    const std::string& app_locale,
    ServerFieldType focused_field_type,
    const std::vector<ServerFieldType>& field_types)
    : LabelFormatter(app_locale, focused_field_type, field_types),
      form_has_street_address_(HasStreetAddress(field_types_for_labels())) {}

AddressEmailFormLabelFormatter::~AddressEmailFormLabelFormatter() {}

// Note that the order--name, address, and email--in which parts of the label
// are added ensures that the label is formatted correctly for |group| and for
// this kind of formatter.
base::string16 AddressEmailFormLabelFormatter::GetLabelForFocusedGroup(
    const AutofillProfile& profile,
    FieldTypeGroup group) const {
  std::vector<base::string16> label_parts;

  if (group != NAME) {
    AddLabelPartIfNotEmpty(GetLabelName(profile, app_locale()), &label_parts);
  }

  if (group != ADDRESS_HOME) {
    AddLabelPartIfNotEmpty(
        GetLabelAddress(form_has_street_address_, profile, app_locale(),
                        field_types_for_labels()),
        &label_parts);
  }

  if (group != EMAIL) {
    AddLabelPartIfNotEmpty(GetLabelEmail(profile, app_locale()), &label_parts);
  }

  return ConstructLabelLine(label_parts);
}

}  // namespace autofill
