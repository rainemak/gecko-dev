/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { Cc, Ci, Cu } = require("chrome");

const { XPCOMUtils } = require("resource://gre/modules/XPCOMUtils.jsm");
const Services = require("Services");

const promise = require("resource://gre/modules/Promise.jsm").Promise;
const { getRuleLocation } = require("devtools/server/actors/stylesheets");

const protocol = require("devtools/server/protocol");
const { method, custom, RetVal, Arg } = protocol;

loader.lazyGetter(this, "gDevTools", () => {
  return require("resource:///modules/devtools/gDevTools.jsm").gDevTools;
});
loader.lazyGetter(this, "DOMUtils", () => {
  return Cc["@mozilla.org/inspector/dom-utils;1"].getService(Ci.inIDOMUtils)
});
loader.lazyGetter(this, "prettifyCSS", () => {
  return require("resource:///modules/devtools/StyleSheetEditor.jsm").prettifyCSS;
});

const CSSRule = Ci.nsIDOMCSSRule;

const MAX_UNUSED_RULES = 10000;

/**
 * Allow: let foo = l10n.lookup("csscoverageFoo");
 */
const l10n = exports.l10n = {
  _URI: "chrome://global/locale/devtools/csscoverage.properties",
  lookup: function(msg) {
    if (this._stringBundle == null) {
      this._stringBundle = Services.strings.createBundle(this._URI);
    }
    return this._stringBundle.GetStringFromName(msg);
  }
};

/**
 * UsageReport manages the collection of CSS usage data.
 * The core of a UsageReport is a JSON-able data structure called _knownRules
 * which looks like this:
 * This records the CSSStyleRules and their usage.
 * The format is:
 *     Map({
 *       <CSS-URL>|<START-LINE>|<START-COLUMN>: {
 *         selectorText: <CSSStyleRule.selectorText>,
 *         test: <simplify(CSSStyleRule.selectorText)>,
 *         cssText: <CSSStyleRule.cssText>,
 *         isUsed: <TRUE|FALSE>,
 *         presentOn: Set([ <HTML-URL>, ... ]),
 *         preLoadOn: Set([ <HTML-URL>, ... ]),
 *         isError: <TRUE|FALSE>,
 *       }
 *     })
 *
 * For example:
 *     this._knownRules = Map({
 *       "http://eg.com/styles1.css|15|0": {
 *         selectorText: "p.quote:hover",
 *         test: "p.quote",
 *         cssText: "p.quote { color: red; }",
 *         isUsed: true,
 *         presentOn: Set([ "http://eg.com/page1.html", ... ]),
 *         preLoadOn: Set([ "http://eg.com/page1.html" ]),
 *         isError: false,
 *       }, ...
 *     });
 */
let UsageReportActor = protocol.ActorClass({
  typeName: "usageReport",

  initialize: function(conn, tabActor) {
    protocol.Actor.prototype.initialize.call(this, conn);

    this._tabActor = tabActor;
    this._running = false;

    this._onTabLoad = this._onTabLoad.bind(this);
    this._onChange = this._onChange.bind(this);
  },

  destroy: function() {
    this._tabActor = undefined;

    delete this._onTabLoad;
    delete this._onChange;

    protocol.Actor.prototype.destroy.call(this);
  },

  /**
   * Begin recording usage data
   */
  start: method(function() {
    if (this._running) {
      throw new Error(l10n.lookup("csscoverageRunningError"));
    }

    this._visitedPages = new Set();
    this._knownRules = new Map();
    this._running = true;
    this._tooManyUnused = false;

    this._tabActor.browser.addEventListener("load", this._onTabLoad, true);

    this._observeMutations(this._tabActor.window.document);

    this._populateKnownRules(this._tabActor.window.document);
    this._updateUsage(this._tabActor.window.document, false);
  }),

  /**
   * Cease recording usage data
   */
  stop: method(function() {
    if (!this._running) {
      throw new Error(l10n.lookup("csscoverageNotRunningError"));
    }

    this._tabActor.browser.removeEventListener("load", this._onTabLoad, true);
    this._running = false;
  }),

  /**
   * Start/stop recording usage data depending on what we're currently doing.
   */
  toggle: method(function() {
    return this._running ?
        this.stop().then(() => false) :
        this.start().then(() => true);
  }, {
    response: RetVal("boolean"),
  }),

  /**
   * Running start() quickly followed by stop() does a bunch of unnecessary
   * work, so this cuts all that out
   */
  oneshot: method(function() {
    if (this._running) {
      throw new Error(l10n.lookup("csscoverageRunningError"));
    }

    this._visitedPages = new Set();
    this._knownRules = new Map();

    this._populateKnownRules(this._tabActor.window.document);
    this._updateUsage(this._tabActor.window.document, false);
  }),

  /**
   * Called from the tab "load" event
   */
  _onTabLoad: function(ev) {
    let document = ev.target;
    this._populateKnownRules(document);
    this._updateUsage(document, true);

    this._observeMutations(document);
  },

  /**
   * Setup a MutationObserver on the current document
   */
  _observeMutations: function(document) {
    let MutationObserver = document.defaultView.MutationObserver;
    let observer = new MutationObserver(mutations => {
      // It's possible that one of the mutations in this list adds a 'use' of
      // a CSS rule, and another takes it away. See Bug 1010189
      this._onChange(document);
    });

    observer.observe(document, {
      attributes: true,
      childList: true,
      characterData: false
    });
  },

  /**
   * Event handler for whenever we think the page has changed in a way that
   * means the CSS usage might have changed.
   */
  _onChange: function(document) {
    // Ignore changes pre 'load'
    if (!this._visitedPages.has(getURL(document))) {
      return;
    }
    this._updateUsage(document, false);
  },

  /**
   * Called whenever we think the list of stylesheets might have changed so
   * we can update the list of rules that we should be checking
   */
  _populateKnownRules: function(document) {
    let url = getURL(document);
    this._visitedPages.add(url);
    // Go through all the rules in the current sheets adding them to knownRules
    // if needed and adding the current url to the list of pages they're on
    for (let rule of getAllSelectorRules(document)) {
      let ruleId = ruleToId(rule);
      let ruleData = this._knownRules.get(ruleId);
      if (ruleData == null) {
        ruleData = {
           selectorText: rule.selectorText,
           cssText: rule.cssText,
           test: getTestSelector(rule.selectorText),
           isUsed: false,
           presentOn: new Set(),
           preLoadOn: new Set(),
           isError: false
        };
        this._knownRules.set(ruleId, ruleData);
      }

      ruleData.presentOn.add(url);
    }
  },

  /**
   * Update knownRules with usage information from the current page
   */
  _updateUsage: function(document, isLoad) {
    let qsaCount = 0;

    // Update this._data with matches to say 'used at load time' by sheet X
    let url = getURL(document);

    for (let [ , ruleData ] of this._knownRules) {
      // If it broke before, don't try again selectors don't change
      if (ruleData.isError) {
        continue;
      }

      // If it's used somewhere already, don't bother checking again unless
      // this is a load event in which case we need to add preLoadOn
      if (!isLoad && ruleData.isUsed) {
        continue;
      }

      // Ignore rules that are not present on this page
      if (!ruleData.presentOn.has(url)) {
        continue;
      }

      qsaCount++;
      if (qsaCount > MAX_UNUSED_RULES) {
        console.error("Too many unused rules on " + url + " ");
        this._tooManyUnused = true;
        continue;
      }

      try {
        let match = document.querySelector(ruleData.test);
        if (match != null) {
          ruleData.isUsed = true;
          if (isLoad) {
            ruleData.preLoadOn.add(url);
          }
        }
      }
      catch (ex) {
        ruleData.isError = true;
      }
    }
  },

  /**
   * Returns a JSONable structure designed to help marking up the style editor,
   * which describes the CSS selector usage.
   * Example:
   *   [
   *     {
   *       selectorText: "p#content",
   *       usage: "unused|used",
   *       start: { line: 3, column: 0 },
   *     },
   *     ...
   *   ]
   */
  createEditorReport: method(function(url) {
    if (this._knownRules == null) {
      return { reports: [] };
    }

    let reports = [];
    for (let [ruleId, ruleData] of this._knownRules) {
      let { url: ruleUrl, line, column } = deconstructRuleId(ruleId);
      if (ruleUrl !== url || ruleData.isUsed) {
        continue;
      }

      let ruleReport = {
        selectorText: ruleData.selectorText,
        start: { line: line, column: column }
      };

      if (ruleData.end) {
        ruleReport.end = ruleData.end;
      }

      reports.push(ruleReport);
    }

    return { reports: reports };
  }, {
    request: { url: Arg(0, "string") },
    response: { reports: RetVal("array:json") }
  }),

  /**
   * Returns a JSONable structure designed for the page report which shows
   * the recommended changes to a page.
   * Example:
   *   {
   *     pages: [
   *      {
   *        url: http://example.org/page1.html,
   *        preloadRules: [
   *          {
   *            url: "http://example.org/style1.css",
   *            start: { line: 3, column: 4 },
   *            selectorText: "p#content",
   *            formattedCssText: "p#content {\n  color: red;\n }\n",
   *            onclick: function() { // open in style editor }
   *          },
   *          ...
   *        ],
   *        unusedRules: [
   *          ...
   *        ]
   *       }
   *     ]
   *   }
   */
  createPageReport: method(function() {
    if (this._running) {
      throw new Error(l10n.lookup("csscoverageRunningError"));
    }

    if (this._visitedPages == null) {
      throw new Error(l10n.lookup("csscoverageNotRunError"));
    }

    // Create a JSONable data structure representing a rule
    const ruleToRuleReport = function(ruleId, ruleData) {
      let { url, line, column } = deconstructRuleId(ruleId);
      return {
        url: url,
        shortHref: url.split("/").slice(-1),
        start: { line: line, column: column },
        selectorText: ruleData.selectorText,
        formattedCssText: prettifyCSS(ruleData.cssText)
      };
    }

    let pages = [];
    let unusedRules = [];

    // Create a set of the unused rules
    for (let [ruleId, ruleData] of this._knownRules) {
      if (!ruleData.isUsed) {
        let ruleReport = ruleToRuleReport(ruleId, ruleData);
        unusedRules.push(ruleReport);
      }
    }

    // Create the set of rules that could be pre-loaded
    for (let url of this._visitedPages) {
      let page = {
        url: url,
        shortHref: url.split("/").slice(-1),
        preloadRules: []
      };

      for (let [ruleId, ruleData] of this._knownRules) {
        if (ruleData.preLoadOn.has(url)) {
          let ruleReport = ruleToRuleReport(ruleId, ruleData);
          page.preloadRules.push(ruleReport);
        }
      }

      if (page.preloadRules.length > 0) {
        pages.push(page);
      }
    }

    return {
      pages: pages,
      unusedRules: unusedRules
    };
  }, {
    response: RetVal("json")
  }),

  /**
   * For testing only. Is css coverage running.
   */
  _testOnly_isRunning: method(function() {
    return this._running;
  }, {
    response: { value: RetVal("boolean")}
  }),

});

exports.UsageReportActor = UsageReportActor;

/**
 * Generator that filters the CSSRules out of _getAllRules so it only
 * iterates over the CSSStyleRules
 */
function* getAllSelectorRules(document) {
  for (let rule of getAllRules(document)) {
    if (rule.type === CSSRule.STYLE_RULE && rule.selectorText !== "") {
      yield rule;
    }
  }
}

/**
 * Generator to iterate over the CSSRules in all the stylesheets the
 * current document (i.e. it includes import rules, media rules, etc)
 */
function* getAllRules(document) {
  // sheets is an array of the <link> and <style> element in this document
  let sheets = getAllSheets(document);
  for (let i = 0; i < sheets.length; i++) {
    for (let j = 0; j < sheets[i].cssRules.length; j++) {
      yield sheets[i].cssRules[j];
    }
  }
}

/**
 * Get an array of all the stylesheets that affect this document. That means
 * the <link> and <style> based sheets, and the @imported sheets (recursively)
 * but not the sheets in nested frames.
 */
function getAllSheets(document) {
  // sheets is an array of the <link> and <style> element in this document
  let sheets = Array.slice(document.styleSheets);
  // Add @imported sheets
  for (let i = 0; i < sheets.length; i++) {
    let subSheets = getImportedSheets(sheets[i]);
    sheets = sheets.concat(...subSheets);
  }
  return sheets;
}

/**
 * Recursively find @import rules in the given stylesheet.
 * We're relying on the browser giving rule.styleSheet == null to resolve
 * @import loops
 */
function getImportedSheets(stylesheet) {
  let sheets = [];
  for (let i = 0; i < stylesheet.cssRules.length; i++) {
    let rule = stylesheet.cssRules[i];
    // rule.styleSheet == null with duplicate @imports for the same URL.
    if (rule.type === CSSRule.IMPORT_RULE && rule.styleSheet != null) {
      sheets.push(rule.styleSheet);
      let subSheets = getImportedSheets(rule.styleSheet);
      sheets = sheets.concat(...subSheets);
    }
  }
  return sheets;
}

/**
 * Get a unique identifier for a rule. This is currently the string
 * <CSS-URL>|<START-LINE>|<START-COLUMN>
 * @see deconstructRuleId(ruleId)
 */
function ruleToId(rule) {
  let loc = getRuleLocation(rule);
  return sheetToUrl(rule.parentStyleSheet) + "|" + loc.line + "|" + loc.column;
}

/**
 * Convert a ruleId to an object with { url, line, column } properties
 * @see ruleToId(rule)
 */
const deconstructRuleId = exports.deconstructRuleId = function(ruleId) {
  let split = ruleId.split("|");
  if (split.length > 3) {
    let replace = split.slice(0, split.length - 3 + 1).join("|");
    split.splice(0, split.length - 3 + 1, replace);
  }
  let [ url, line, column ] = split;
  return {
    url: url,
    line: parseInt(line, 10),
    column: parseInt(column, 10)
  };
};

/**
 * We're only interested in the origin and pathname, because changes to the
 * username, password, hash, or query string probably don't significantly
 * change the CSS usage properties of a page.
 * @param document
 */
const getURL = exports.getURL = function(document) {
  let url = new document.defaultView.URL(document.documentURI);
  return '' + url.origin + url.pathname;
};

/**
 * Pseudo class handling constants:
 * We split pseudo-classes into a number of categories so we can decide how we
 * should match them. See getTestSelector for how we use these constants.
 *
 * @see http://dev.w3.org/csswg/selectors4/#overview
 * @see https://developer.mozilla.org/en-US/docs/tag/CSS%20Pseudo-class
 * @see https://developer.mozilla.org/en-US/docs/Web/CSS/Pseudo-elements
 */

/**
 * Category 1: Pseudo-classes that depend on external browser/OS state
 * This includes things like the time, locale, position of mouse/caret/window,
 * contents of browser history, etc. These can be hard to mimic.
 * Action: Remove from selectors
 */
const SEL_EXTERNAL = [
  "active", "active-drop", "current", "dir", "focus", "future", "hover",
  "invalid-drop",  "lang", "past", "placeholder-shown", "target", "valid-drop",
  "visited"
];

/**
 * Category 2: Pseudo-classes that depend on user-input state
 * These are pseudo-classes that arguably *should* be covered by unit tests but
 * which probably aren't and which are unlikely to be covered by manual tests.
 * We're currently stripping them out,
 * Action: Remove from selectors (but consider future command line flag to
 * enable them in the future. e.g. 'csscoverage start --strict')
 */
const SEL_FORM = [
  "checked", "default", "disabled", "enabled", "fullscreen", "in-range",
  "indeterminate", "invalid", "optional", "out-of-range", "required", "valid"
];

/**
 * Category 3: Pseudo-elements
 * querySelectorAll doesn't return matches with pseudo-elements because there
 * is no element to match (they're pseudo) so we have to remove them all.
 * (See http://codepen.io/joewalker/pen/sanDw for a demo)
 * Action: Remove from selectors (including deprecated single colon versions)
 */
const SEL_ELEMENT = [
  "after", "before", "first-letter", "first-line", "selection"
];

/**
 * Category 4: Structural pseudo-classes
 * This is a category defined by the spec (also called tree-structural and
 * grid-structural) for selection based on relative position in the document
 * tree that cannot be represented by other simple selectors or combinators.
 * Action: Require a page-match
 */
const SEL_STRUCTURAL = [
  "empty", "first-child", "first-of-type", "last-child", "last-of-type",
  "nth-column", "nth-last-column", "nth-child", "nth-last-child",
  "nth-last-of-type", "nth-of-type", "only-child", "only-of-type", "root"
];

/**
 * Category 4a: Semi-structural pseudo-classes
 * These are not structural according to the spec, but act nevertheless on
 * information in the document tree.
 * Action: Require a page-match
 */
const SEL_SEMI = [ "any-link", "link", "read-only", "read-write", "scope" ];

/**
 * Category 5: Combining pseudo-classes
 * has(), not() etc join selectors together in various ways. We take care when
 * removing pseudo-classes to convert "not(:hover)" into "not(*)" and so on.
 * With these changes the combining pseudo-classes should probably stand on
 * their own.
 * Action: Require a page-match
 */
const SEL_COMBINING = [ "not", "has", "matches" ];

/**
 * Category 6: Media pseudo-classes
 * Pseudo-classes that should be ignored because they're only relevant to
 * media queries
 * Action: Don't need removing from selectors as they appear in media queries
 */
const SEL_MEDIA = [ "blank", "first", "left", "right" ];

/**
 * A test selector is a reduced form of a selector that we actually test
 * against. This code strips out pseudo-elements and some pseudo-classes that
 * we think should not have to match in order for the selector to be relevant.
 */
function getTestSelector(selector) {
  let replaceSelector = pseudo => {
    selector = selector.replace(" :" + selector, " *")
                       .replace("(:" + selector, "(*")
                       .replace(":" + selector, "");
  };

  SEL_EXTERNAL.forEach(replaceSelector);
  SEL_FORM.forEach(replaceSelector);
  SEL_ELEMENT.forEach(replaceSelector);

  // Pseudo elements work in : and :: forms
  SEL_ELEMENT.forEach(pseudo => {
    selector = selector.replace("::" + selector, "");
  });

  return selector;
}

/**
 * I've documented all known pseudo-classes above for 2 reasons: To allow
 * checking logic and what might be missing, but also to allow a unit test
 * that fetches the list of supported pseudo-classes and pseudo-elements from
 * the platform and check that they were all represented here.
 */
exports.SEL_ALL = [
  SEL_EXTERNAL, SEL_FORM, SEL_ELEMENT, SEL_STRUCTURAL, SEL_SEMI,
  SEL_COMBINING, SEL_MEDIA
].reduce(function(prev, curr) { return prev.concat(curr); }, []);

/**
 * Find a URL for a given stylesheet
 */
const sheetToUrl = exports.sheetToUrl = function(stylesheet) {
  if (stylesheet.href) {
    return stylesheet.href;
  }
  if (stylesheet.ownerNode && stylesheet.ownerNode.baseURI) {
    return stylesheet.ownerNode.baseURI;
  }

  throw new Error("Unknown sheet source");
}

/**
 * Front for UsageReportActor
 */
const UsageReportFront = protocol.FrontClass(UsageReportActor, {
  initialize: function(client, form) {
    protocol.Front.prototype.initialize.call(this, client, form);
    this.actorID = form.usageReportActor;
    this.manage(this);
  },

  start: custom(function(chromeWindow, target) {
    if (chromeWindow != null) {
      let gnb = chromeWindow.document.getElementById("global-notificationbox");
      let notification = gnb.getNotificationWithValue("csscoverage-running");

      if (!notification) {
        let notifyStop = ev => {
          if (ev == "removed") {
            this.stop();
            gDevTools.showToolbox(target, "styleeditor");
          }
        };

        gnb.appendNotification(l10n.lookup("csscoverageRunningReply"),
                               "csscoverage-running",
                               "", // i.e. no image
                               gnb.PRIORITY_INFO_HIGH,
                               null, // i.e. no buttons
                               notifyStop);
      }
    }

    return this._start();
  }, {
    impl: "_start"
  })
});

exports.UsageReportFront = UsageReportFront;

/**
 * Registration / De-registration
 */
exports.register = function(handle) {
  handle.addGlobalActor(UsageReportActor, "usageReportActor");
  handle.addTabActor(UsageReportActor, "usageReportActor");
};

exports.unregister = function(handle) {
  handle.removeGlobalActor(UsageReportActor, "usageReportActor");
  handle.removeTabActor(UsageReportActor, "usageReportActor");
};

const knownFronts = new WeakMap();

/**
 * Create a UsageReportFront only when needed (returns a promise)
 */
const getUsage = exports.getUsage = function(target) {
  return target.makeRemote().then(() => {
    let front = knownFronts.get(target.client)
    if (front == null) {
      front = new UsageReportFront(target.client, target.form);
      knownFronts.set(target.client, front);
    }
    return front;
  });
};
