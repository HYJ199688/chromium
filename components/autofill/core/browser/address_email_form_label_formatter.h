// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_CORE_BROWSER_ADDRESS_EMAIL_FORM_LABEL_FORMATTER_H_
#define COMPONENTS_AUTOFILL_CORE_BROWSER_ADDRESS_EMAIL_FORM_LABEL_FORMATTER_H_

#include <string>
#include <vector>

#include "base/strings/string16.h"
#include "components/autofill/core/browser/autofill_profile.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/label_formatter.h"

namespace autofill {

// A LabelFormatter that creates Suggestions' disambiguating labels for forms
// with name, address, and email fields and without phone fields.
class AddressEmailFormLabelFormatter : public LabelFormatter {
 public:
  AddressEmailFormLabelFormatter(
      const std::string& app_locale,
      ServerFieldType focused_field_type,
      const std::vector<ServerFieldType>& field_types);

  ~AddressEmailFormLabelFormatter() override;

  base::string16 GetLabelForFocusedGroup(const AutofillProfile& profile,
                                         FieldTypeGroup group) const override;

 private:
  // True if this formatter's associated form has a street address field. A
  // form may have an address-related field, e.g. zip code, without having a
  // street address field. If a form does not include a street address field,
  // street addresses should not appear in labels.
  bool form_has_street_address_;
};

}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_BROWSER_ADDRESS_EMAIL_FORM_LABEL_FORMATTER_H_