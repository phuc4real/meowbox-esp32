//! JSON helpers. `parse_unique` refuses an object with a repeated key at any depth (Python's `unique_keys`
//! object_pairs_hook): `{"t":"busy","t":"media_cmd"}` could mean two different things, so it is dropped.

use serde::de::{self, Deserialize, Deserializer, MapAccess, SeqAccess, Visitor};
use serde_json::{Map, Value};
use std::fmt;

struct Unique(Value);

impl<'de> Deserialize<'de> for Unique {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V;
        impl<'de> Visitor<'de> for V {
            type Value = Unique;
            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("JSON")
            }
            fn visit_bool<E>(self, v: bool) -> Result<Unique, E> {
                Ok(Unique(Value::Bool(v)))
            }
            fn visit_i64<E>(self, v: i64) -> Result<Unique, E> {
                Ok(Unique(v.into()))
            }
            fn visit_u64<E>(self, v: u64) -> Result<Unique, E> {
                Ok(Unique(v.into()))
            }
            fn visit_f64<E>(self, v: f64) -> Result<Unique, E> {
                Ok(Unique(serde_json::Number::from_f64(v).map_or(Value::Null, Value::Number)))
            }
            fn visit_str<E>(self, v: &str) -> Result<Unique, E> {
                Ok(Unique(Value::String(v.to_owned())))
            }
            fn visit_string<E>(self, v: String) -> Result<Unique, E> {
                Ok(Unique(Value::String(v)))
            }
            fn visit_unit<E>(self) -> Result<Unique, E> {
                Ok(Unique(Value::Null))
            }
            fn visit_seq<A: SeqAccess<'de>>(self, mut a: A) -> Result<Unique, A::Error> {
                let mut v = Vec::new();
                while let Some(Unique(x)) = a.next_element()? {
                    v.push(x);
                }
                Ok(Unique(Value::Array(v)))
            }
            fn visit_map<A: MapAccess<'de>>(self, mut a: A) -> Result<Unique, A::Error> {
                let mut m = Map::new();
                while let Some(k) = a.next_key::<String>()? {
                    let Unique(v) = a.next_value()?;
                    if m.insert(k.clone(), v).is_some() {
                        return Err(de::Error::custom(format!("repeated key {k:?}")));
                    }
                }
                Ok(Unique(Value::Object(m)))
            }
        }
        d.deserialize_any(V)
    }
}

/// Parse JSON, refusing repeated keys in any object.
pub fn parse_unique(text: &str) -> serde_json::Result<Value> {
    serde_json::from_str::<Unique>(text).map(|u| u.0)
}

/// Compact JSON, non-ASCII kept as is (Python: `json.dumps(ensure_ascii=False, separators=(",", ":"))`).
pub fn compact(v: &Value) -> String {
    v.to_string()
}

/// Python's round(): halves to even, so stats round the same way as the Python agent did.
pub fn round(x: f64) -> i64 {
    x.round_ties_even() as i64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repeated_key_is_refused_at_any_depth() {
        assert!(parse_unique(r#"{"t":"media_cmd","cmd":"next"}"#).is_ok());
        assert!(parse_unique(r#"{"t":"busy","t":"media_cmd"}"#).is_err());
        assert!(parse_unique(r#"{"t":"x","a":{"b":1,"b":2}}"#).is_err());
        assert!(parse_unique(r#"[{"a":1},{"a":2}]"#).is_ok());
    }

    #[test]
    fn compact_keeps_unicode() {
        let v = serde_json::json!({"t": "media", "title": "Đêm trăng"});
        assert_eq!(compact(&v), r#"{"t":"media","title":"Đêm trăng"}"#);
    }

    #[test]
    fn rounds_like_python() {
        assert_eq!((round(2.5), round(3.5), round(-0.5), round(2.6)), (2, 4, 0, 3));
    }
}
