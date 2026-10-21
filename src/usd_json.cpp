#include "usd_json.hpp"

#include "duckdb/common/operator/convert_to_string.hpp"
#include "utf8proc_wrapper.hpp"

#include <pxr/base/gf/half.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/base/vt/typeHeaders.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/visitValue.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/timeCode.h>

#include <cmath>
#include <cstring>
#include <type_traits>
#include <typeinfo>

namespace duckdb {

namespace {

template <class T, class = void>
struct IsGfVec : std::false_type {};
template <class T>
struct IsGfVec<T, std::void_t<decltype(T::dimension), typename T::ScalarType>> : std::true_type {};

template <class T, class = void>
struct IsGfMatrix : std::false_type {};
template <class T>
struct IsGfMatrix<T, std::void_t<decltype(T::numRows), decltype(T::numColumns)>> : std::true_type {};

template <class T, class = void>
struct IsGfQuat : std::false_type {};
template <class T>
struct IsGfQuat<T, std::void_t<decltype(std::declval<T>().GetReal()), decltype(std::declval<T>().GetImaginary())>>
    : std::true_type {};

template <class T, class = void>
struct IsGfRange : std::false_type {};
template <class T>
struct IsGfRange<T, std::void_t<typename T::MinMaxType, decltype(std::declval<T>().GetMin())>> : std::true_type {};

//! Appends a float or double. JSON has no NaN or infinity: they become null.
template <class T>
void AppendNumber(T value, string &out) {
	if (!std::isfinite(value)) {
		out += "null";
		return;
	}
	out += ConvertToString::Operation(value);
}

//! Appends a JSON string literal. JSON must be valid UTF-8: invalid bytes are replaced with '?'.
void AppendString(const std::string &value, string &out) {
	static const char *HEX = "0123456789abcdef";
	std::string valid;
	auto data = value.data();
	if (Utf8Proc::Analyze(data, value.size()) == UnicodeType::INVALID) {
		valid = value;
		Utf8Proc::MakeValid(&valid[0], valid.size());
		data = valid.data();
	}
	auto bytes = reinterpret_cast<const unsigned char *>(data);
	out += '"';
	for (idx_t i = 0; i < value.size(); i++) {
		auto c = bytes[i];
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += HEX[c >> 4];
				out += HEX[c & 0xF];
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

struct JsonVisitor {
	string &out;

	template <class T>
	void operator()(const pxr::VtArray<T> &array) {
		Sequence(array);
	}

	template <class T>
	void operator()(const std::vector<T> &values) {
		Sequence(values);
	}

	template <class T>
	void operator()(const T &value) {
		Element(value);
	}

	void operator()(const pxr::VtValue &value) {
		if (value.IsEmpty()) {
			out += "null";
			return;
		}
		// types that VtVisitValue does not dispatch on its own
		if (Holding<pxr::VtDictionary>(value) || Holding<pxr::SdfAssetPath>(value) ||
		    Holding<pxr::VtArray<pxr::SdfAssetPath>>(value) || Holding<pxr::SdfTimeCode>(value) ||
		    Holding<pxr::VtArray<pxr::SdfTimeCode>>(value) || Holding<pxr::SdfPath>(value) ||
		    Holding<pxr::SdfPathVector>(value) || Holding<std::vector<pxr::VtValue>>(value)) {
			return;
		}
		AppendString(pxr::TfStringify(value), out);
	}

	//! Writes `value` if it holds a T
	template <class T>
	bool Holding(const pxr::VtValue &value) {
		// IsHolding compares type_info objects, but a type whose type_info OpenUSD does not export (e.g.
		// SdfAssetPath) has a separate copy in this library - so fall back to comparing the mangled names
		if (!value.IsHolding<T>() && std::strcmp(value.GetTypeid().name(), typeid(T).name()) != 0) {
			return false;
		}
		(*this)(value.UncheckedGet<T>());
		return true;
	}

	template <class SEQUENCE>
	void Sequence(const SEQUENCE &values) {
		out += '[';
		for (size_t i = 0; i < values.size(); i++) {
			if (i > 0) {
				out += ',';
			}
			Element(values[i]);
		}
		out += ']';
	}

	template <class T>
	void Element(const T &value) {
		if constexpr (std::is_same<T, bool>::value) {
			out += value ? "true" : "false";
		} else if constexpr (std::is_same<T, pxr::GfHalf>::value) {
			AppendNumber(static_cast<float>(value), out);
		} else if constexpr (std::is_floating_point<T>::value) {
			AppendNumber(value, out);
		} else if constexpr (std::is_integral<T>::value) {
			out += std::to_string(value);
		} else if constexpr (std::is_same<T, std::string>::value) {
			AppendString(value, out);
		} else if constexpr (std::is_same<T, pxr::TfToken>::value || std::is_same<T, pxr::SdfPath>::value) {
			AppendString(value.GetString(), out);
		} else if constexpr (std::is_same<T, pxr::SdfAssetPath>::value) {
			AppendString(value.GetAssetPath(), out);
		} else if constexpr (std::is_same<T, pxr::SdfTimeCode>::value) {
			AppendNumber(value.GetValue(), out);
		} else if constexpr (std::is_same<T, pxr::VtValue>::value) {
			UsdJson::Append(value, out);
		} else if constexpr (std::is_same<T, pxr::VtDictionary>::value) {
			out += '{';
			bool first = true;
			for (auto &entry : value) {
				if (!first) {
					out += ',';
				}
				first = false;
				AppendString(entry.first, out);
				out += ':';
				UsdJson::Append(entry.second, out);
			}
			out += '}';
		} else if constexpr (IsGfRange<T>::value) {
			// checked before vectors: ranges also declare a dimension and scalar type
			out += "{\"min\":";
			Element(value.GetMin());
			out += ",\"max\":";
			Element(value.GetMax());
			out += '}';
		} else if constexpr (IsGfVec<T>::value) {
			out += '[';
			for (size_t i = 0; i < T::dimension; i++) {
				if (i > 0) {
					out += ',';
				}
				Element(value[i]);
			}
			out += ']';
		} else if constexpr (IsGfMatrix<T>::value) {
			out += '[';
			for (size_t row = 0; row < T::numRows; row++) {
				out += row > 0 ? ",[" : "[";
				for (size_t col = 0; col < T::numColumns; col++) {
					if (col > 0) {
						out += ',';
					}
					Element(value[row][col]);
				}
				out += ']';
			}
			out += ']';
		} else if constexpr (IsGfQuat<T>::value) {
			out += "{\"real\":";
			Element(value.GetReal());
			out += ",\"imaginary\":";
			Element(value.GetImaginary());
			out += '}';
		} else {
			AppendString(pxr::TfStringify(value), out);
		}
	}
};

} // namespace

void UsdJson::Append(const pxr::VtValue &value, string &out) {
	pxr::VtVisitValue(value, JsonVisitor {out});
}

idx_t UsdJson::ArrayLength(const pxr::VtValue &value) {
	if (value.IsEmpty() || !value.IsArrayValued()) {
		return DConstants::INVALID_INDEX;
	}
	return value.GetArraySize();
}

} // namespace duckdb
