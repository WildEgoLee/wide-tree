//! Visible-row projection for an ultra-wide tree.
//!
//! v1 freezes three constraints: immutable snapshots, fixed child order, and
//! fixed row height. Collapsing a node clears only that node's expanded
//! preference; descendant preferences stay.

pub mod types;
pub mod snapshot;
pub mod builder;
pub mod view;

pub use types::*;
pub use snapshot::TreeSnapshot;
pub use builder::{TreeBuilder, BuildResult, BuildError};
pub use view::{TreeView, ViewConfig, ViewResult};
