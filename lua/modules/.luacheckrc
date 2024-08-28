return {
	max_line_length = false,
	allow_defined = true,
	ignore = {
		"421", -- Shadowing a local variable.
		"422", -- Shadowing an argument.
		"423", -- Shadowing a loop variable.
		"431", -- Shadowing an upvalue.
		"432", -- Shadowing an upvalue argument.
		"433", -- Shadowing an upvalue loop variable.
	},

	globals = {
		"Camera",
		"Client",
		"Clouds",
		"Config",
		"Dev",
		"Fog",
		"Light",
		"Menu",
		"Player",
		"Pointer",
		"Screen",
		"Sky",
		"System",
		"CollisionGroups",
		"Data",
	},

	read_globals = {
		"Animation",
		"Assets",
		"AssetType",
		"AudioListener",
		"AudioSource",
		"Box",
		"Color",
		"Event",
		"Environment",
		"Face",
		"File",
		"Font",
		"HTTP",
		"Items",
		"JSON",
		"KeyValueStore",
		"LightType",
		"LocalEvent",
		"Map",
		"MutableShape",
		"Number2",
		"Number3",
		"Object",
		"OtherPlayers",
		"PhysicsMode",
		"Players",
		"PointerEvent",
		"Private",
		"ProjectionMode",
		"Quad",
		"Ray",
		"Rotation",
		"Shape",
		"Text",
		"TextType",
		"Time",
		"Timer",
		"Type",
		"URL",
		"World",
		"math.clamp"
	}
}
