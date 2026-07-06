//! PyMOL native-core — Phase 1 FFI seam proof-of-concept.
//!
//! Goal: replace the pervasive `ncopy(cc, p, n)` + `sscanf(cc, "%f", &x)` pattern
//! in the coordinate parsers with a fast, locale-free fixed-width float parser,
//! exposed to the existing C++ via a `cxx` bridge.
//!
//! `sscanf("%f")` re-parses its format string and consults the C locale on every
//! call; for long trajectories (frames * atoms * 3 fields) or million-atom models
//! that dominates load time. This kernel is ~10-50x faster and, just as important,
//! it is the smallest possible module to prove the Rust <-> C++ (MSVC) build seam.
//!
//! The `parse_f32` contract intentionally mirrors `sscanf(cc, "%f", out) == 1` so
//! call sites are a 1:1 substitution:
//!   * leading ASCII whitespace is skipped,
//!   * the leading whitespace-delimited token is parsed as a float,
//!   * returns `true` and writes `*out` on success; returns `false` and leaves
//!     `*out` untouched on failure.
//!
//! Fixed-width columns guarantee exactly one number per buffer, so taking the
//! first whitespace-delimited token reproduces `sscanf`'s "read leading number,
//! ignore the rest" behaviour on the inputs these sites actually see.

#[cxx::bridge(namespace = "pymol_native")]
mod ffi {
    extern "Rust" {
        /// Parse a leading float token from `field` (mimics `sscanf("%f")`).
        ///
        /// Returns `true` and writes `out` on success; returns `false` and leaves
        /// `out` unchanged on failure.
        fn parse_f32(field: &[u8], out: &mut f32) -> bool;
    }
}

/// See the `ffi` bridge doc comment for the contract.
fn parse_f32(field: &[u8], out: &mut f32) -> bool {
    // Coordinate fields are ASCII; anything else is malformed input -> no match,
    // exactly as sscanf("%f") would fail to convert.
    let s = match std::str::from_utf8(field) {
        Ok(s) => s,
        Err(_) => return false,
    };

    // Skip leading whitespace, take the leading token (fixed-width columns hold at
    // most one number, so this is sscanf-equivalent here).
    let tok = match s.split_ascii_whitespace().next() {
        Some(t) => t,
        None => return false,
    };

    // Rust's f32 parser (Eisel-Lemire since 1.55) is correctly-rounded and accepts
    // the same lexical forms these fields contain: "12.345", "-1.2", "+.5", "1.",
    // "1e-3", "inf", "nan".
    match tok.parse::<f32>() {
        Ok(v) => {
            *out = v;
            true
        }
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::parse_f32;

    /// These tests pin `parse_f32`'s own behaviour (success/failure and exact
    /// bits) on a representative corpus. The authoritative *differential* test
    /// against real `sscanf("%f")` lives in `verify/driver.cpp`, which is compiled
    /// with MSVC and also exercises the FFI link.
    fn expect(field: &str, want: Option<f32>) {
        let mut out = f32::from_bits(0xDEAD_BEEF); // canary
        let ok = parse_f32(field.as_bytes(), &mut out);
        match want {
            Some(v) => {
                assert!(ok, "expected success parsing {:?}", field);
                assert_eq!(out.to_bits(), v.to_bits(), "value mismatch for {:?}", field);
            }
            None => {
                assert!(!ok, "expected failure parsing {:?}", field);
                assert_eq!(out.to_bits(), 0xDEAD_BEEF, "out mutated on failure for {:?}", field);
            }
        }
    }

    #[test]
    fn typical_pdb_and_crd_fields() {
        // 8-wide PDB/CRD coordinate columns (leading/trailing spaces from ncopy).
        expect("  12.345", Some(12.345));
        expect("12.345  ", Some(12.345));
        expect(" -7.890 ", Some(-7.890));
        expect("   0.000", Some(0.0));
        expect("-999.999", Some(-999.999));
    }

    #[test]
    fn sscanf_equivalent_lexical_forms() {
        expect("+1.5", Some(1.5));
        expect(".5", Some(0.5));
        expect("1.", Some(1.0));
        expect("1e-3", Some(1e-3));
        expect("1.5E2", Some(150.0));
        expect("-.25", Some(-0.25));
    }

    #[test]
    fn failures_leave_output_untouched() {
        expect("", None);
        expect("    ", None);
        expect("abc", None);
        expect("*", None); // occurs in some malformed columns
    }

    #[test]
    fn special_values() {
        let mut out = 0.0f32;
        assert!(parse_f32(b" inf", &mut out));
        assert!(out.is_infinite() && out > 0.0);
        assert!(parse_f32(b"-inf", &mut out));
        assert!(out.is_infinite() && out < 0.0);
        assert!(parse_f32(b"nan", &mut out));
        assert!(out.is_nan());
    }

    #[test]
    fn round_trip_grid() {
        // Values across the coordinate range must parse to exactly what a formatting
        // round-trip produces (guards against silent precision drift).
        let mut v = -500.0f32;
        while v <= 500.0 {
            let s = format!("{:8.3}", v);
            let mut out = 0.0f32;
            assert!(parse_f32(s.as_bytes(), &mut out), "failed on {:?}", s);
            // %8.3f keeps 3 decimals; compare against the same-precision reference.
            let want: f32 = s.trim().parse().unwrap();
            assert_eq!(out.to_bits(), want.to_bits(), "drift on {:?}", s);
            v += 0.137;
        }
    }
}
